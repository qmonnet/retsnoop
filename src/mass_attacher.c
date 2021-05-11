// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (c) 2021 Facebook */
#include <errno.h>
#include <stdbool.h>
#include <bpf/btf.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <sys/resource.h>
#include "mass_attacher.h"
#include "ksyms.h"

#ifndef SKEL_NAME
#error "Please define -DSKEL_NAME=<BPF skeleton name> for mass_attacher"
#endif
#ifndef SKEL_HEADER
#error "Please define -DSKEL_HEADER=<path to .skel.h> for mass_attacher"
#endif

#define ____resolve(x) #x
#define ___resolve(x) ____resolve(x)

/* Some skeletons expect common (between BPF and user-space parts of the
 * application) header with extra types. SKEL_EXTRA_HEADER, if specified, will
 * be included to get those types defined to make it possible to compile full
 * BPF skeleton definition properly.
 */
#ifdef SKEL_EXTRA_HEADER
#include ___resolve(SKEL_EXTRA_HEADER)
#endif
#include ___resolve(SKEL_HEADER)

#define ___concat(a, b) a ## b
#define ___apply(fn, n) ___concat(fn, n)

#define SKEL_LOAD(skel) ___apply(SKEL_NAME, __load)(skel)
#define SKEL_ATTACH(skel) ___apply(SKEL_NAME, __attach)(skel)
#define SKEL_DESTROY(skel) ___apply(SKEL_NAME, __destroy)(skel)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

static const char *enforced_deny_globs[] = {
	/* we use it for recursion protection */
	"bpf_get_smp_processor_id",

	/* low-level delicate functions */
	"migrate_enable",
	"migrate_disable",
	"rcu_read_lock*",
	"rcu_read_unlock*",
	"__bpf_prog_enter*",
	"__bpf_prog_exit*",

	/* long-sleeping syscalls, avoid attaching to them unless kernel has
	 * e21aa341785c ("bpf: Fix fexit trampoline.")
	 * TODO: check the presence of above commit and allow long-sleeping
	 * functions.
	 */
	"*_sys_select",
	"*_sys_epoll_wait",
	"*_sys_ppoll",
};

#define MAX_FUNC_ARG_CNT 11

struct mass_attacher;

static _Thread_local struct mass_attacher *cur_attacher;

struct mass_attacher {
	struct ksyms *ksyms;
	struct btf *vmlinux_btf;
	struct SKEL_NAME *skel;

	struct bpf_program *fentries[MAX_FUNC_ARG_CNT + 1];
	struct bpf_program *fexits[MAX_FUNC_ARG_CNT + 1];
	struct bpf_insn *fentries_insns[MAX_FUNC_ARG_CNT + 1];
	struct bpf_insn *fexits_insns[MAX_FUNC_ARG_CNT + 1];
	size_t fentries_insn_cnts[MAX_FUNC_ARG_CNT + 1];
	size_t fexits_insn_cnts[MAX_FUNC_ARG_CNT + 1];

	bool verbose;
	bool debug;
	bool debug_extra;
	int max_func_cnt;
	int max_fileno_rlimit;
	func_filter_fn func_filter;

	struct mass_attacher_func_info *func_infos;
	int func_cnt;

	int func_info_cnts[MAX_FUNC_ARG_CNT + 1];
	int func_info_id_for_arg_cnt[MAX_FUNC_ARG_CNT + 1];

	char **kprobes;
	int kprobe_cnt;

	int allow_glob_cnt;
	int deny_glob_cnt;
	struct {
		char *glob;
		int matches;
	} *allow_globs, *deny_globs;
};

struct mass_attacher *mass_attacher__new(struct SKEL_NAME *skel, struct mass_attacher_opts *opts)
{
	struct mass_attacher *att;
	int i, err;

	if (!skel)
		return NULL;

	att = calloc(1, sizeof(*att));
	if (!att)
		return NULL;

	att->skel = skel;

	if (!opts)
		return att;

	att->max_func_cnt = opts->max_func_cnt;
	att->max_fileno_rlimit = opts->max_fileno_rlimit;
	att->verbose = opts->verbose;
	att->debug = opts->debug;
	att->debug_extra = opts->debug_extra;
	if (att->debug)
		att->verbose = true;
	att->func_filter = opts->func_filter;

	for (i = 0; i < ARRAY_SIZE(enforced_deny_globs); i++) {
		err = mass_attacher__deny_glob(att, enforced_deny_globs[i]);
		if (err) {
			fprintf(stderr, "Failed to add enforced deny glob '%s': %d\n",
				enforced_deny_globs[i], err);
			mass_attacher__free(att);
			return NULL;
		}
	}

	return att;
}

void mass_attacher__free(struct mass_attacher *att)
{
	int i;

	if (!att)
		return;

	if (att->skel)
		att->skel->bss->ready = false;

	ksyms__free(att->ksyms);
	btf__free(att->vmlinux_btf);

	free(att->func_infos);

	if (att->kprobes) {
		for (i = 0; i < att->kprobe_cnt; i++)
			free(att->kprobes[i]);
		free(att->kprobes);
	}

	for (i = 0; i <= MAX_FUNC_ARG_CNT; i++) {
		free(att->fentries_insns[i]);
		free(att->fexits_insns[i]);
	}

	SKEL_DESTROY(att->skel);

	free(att);
}

static bool is_valid_glob(const char *glob)
{
	int i, n;

	if (!glob) {
		fprintf(stderr, "NULL glob provided.\n");
		return false;
	}
	
	n = strlen(glob);
	if (n == 0) {
		fprintf(stderr, "Empty glob provided.\n");
		return false;
	}

	for (i = 0; i < n; i++) {
		if (glob[i] == '*' && i != 0 && i != n - 1) {
			fprintf(stderr,
				"Unsupported glob '%s': '*' allowed only at the beginning or end of a glob.\n",
				glob);
			return false;
		}
	}

	if (strcmp(glob, "**") == 0) {
		fprintf(stderr, "Unsupported glob '%s'.\n", glob);
		return false;
	}

	return true;
}

int mass_attacher__allow_glob(struct mass_attacher *att, const char *glob)
{
	void *tmp, *s;

	if (!is_valid_glob(glob))
		return -EINVAL;

	tmp = realloc(att->allow_globs, (att->allow_glob_cnt + 1) * sizeof(*att->allow_globs));
	if (!tmp)
		return -ENOMEM;
	att->allow_globs = tmp;

	s = strdup(glob);
	if (!s)
		return -ENOMEM;

	att->allow_globs[att->allow_glob_cnt].glob = s;
	att->allow_globs[att->allow_glob_cnt].matches = 0;
	att->allow_glob_cnt++;

	return 0;
}

int mass_attacher__deny_glob(struct mass_attacher *att, const char *glob)
{
	void *tmp, *s;

	if (!is_valid_glob(glob))
		return -EINVAL;

	tmp = realloc(att->deny_globs, (att->deny_glob_cnt + 1) * sizeof(*att->deny_globs));
	if (!tmp)
		return -ENOMEM;
	att->deny_globs = tmp;

	s = strdup(glob);
	if (!s)
		return -ENOMEM;

	att->deny_globs[att->deny_glob_cnt].glob = s;
	att->deny_globs[att->deny_glob_cnt].matches = 0;
	att->deny_glob_cnt++;


	return 0;
}

static int bump_rlimit(int resource, rlim_t max);
static int load_available_kprobes(struct mass_attacher *attacher);
static int hijack_prog(struct bpf_program *prog, int n,
		       struct bpf_insn *insns, int insns_cnt,
		       struct bpf_prog_prep_result *res);

static int func_arg_cnt(const struct btf *btf, int id);
static bool is_kprobe_ok(const struct mass_attacher *att, const char *name);
static bool is_func_type_ok(const struct btf *btf, const struct btf_type *t);

int mass_attacher__prepare(struct mass_attacher *att)
{
	int err, i, j, n;
	int func_skip = 0;
	void *tmp;

	/* Load and cache /proc/kallsyms for IP <-> kfunc mapping */
	att->ksyms = ksyms__load();
	if (!att->ksyms) {
		fprintf(stderr, "Failed to load /proc/kallsyms\n");
		return -EINVAL;
	}

	/* Bump RLIMIT_MEMLOCK to allow BPF sub-system to do anything */
	err = bump_rlimit(RLIMIT_MEMLOCK, RLIM_INFINITY);
	if (err) {
		fprintf(stderr, "Failed to set RLIM_MEMLOCK. Won't be able to load BPF programs: %d\n", err);
		return err;
	}

	/* Allow opening lots of BPF programs */
	err = bump_rlimit(RLIMIT_NOFILE, att->max_fileno_rlimit ?: 300000);
	if (err) {
		fprintf(stderr, "Failed to set RLIM_NOFILE. Won't be able to attach many BPF programs: %d\n", err);
		return err;
	}

	/* Load names of possible kprobes */
	err = load_available_kprobes(att);
	if (err) {
		fprintf(stderr, "Failed to read the list of available kprobes: %d\n", err);
		return err;
	}

	_Static_assert(MAX_FUNC_ARG_CNT == 11, "Unexpected maximum function arg count");
	att->fentries[0] = att->skel->progs.fentry0;
	att->fentries[1] = att->skel->progs.fentry1;
	att->fentries[2] = att->skel->progs.fentry2;
	att->fentries[3] = att->skel->progs.fentry3;
	att->fentries[4] = att->skel->progs.fentry4;
	att->fentries[5] = att->skel->progs.fentry5;
	att->fentries[6] = att->skel->progs.fentry6;
	att->fentries[7] = att->skel->progs.fentry7;
	att->fentries[8] = att->skel->progs.fentry8;
	att->fentries[9] = att->skel->progs.fentry9;
	att->fentries[10] = att->skel->progs.fentry10;
	att->fentries[11] = att->skel->progs.fentry11;
	att->fexits[0] = att->skel->progs.fexit0;
	att->fexits[1] = att->skel->progs.fexit1;
	att->fexits[2] = att->skel->progs.fexit2;
	att->fexits[3] = att->skel->progs.fexit3;
	att->fexits[4] = att->skel->progs.fexit4;
	att->fexits[5] = att->skel->progs.fexit5;
	att->fexits[6] = att->skel->progs.fexit6;
	att->fexits[7] = att->skel->progs.fexit7;
	att->fexits[8] = att->skel->progs.fexit8;
	att->fexits[9] = att->skel->progs.fexit9;
	att->fexits[10] = att->skel->progs.fexit10;
	att->fexits[11] = att->skel->progs.fexit11;

	att->vmlinux_btf = libbpf_find_kernel_btf();
	err = libbpf_get_error(att->vmlinux_btf);
	if (err) {
		fprintf(stderr, "Failed to load vmlinux BTF: %d\n", err);
		return -EINVAL;
	}

	n = btf__get_nr_types(att->vmlinux_btf);
	for (i = 1; i <= n; i++) {
		const struct btf_type *t = btf__type_by_id(att->vmlinux_btf, i);
		const char *func_name;
		const struct ksym *ksym;
		struct mass_attacher_func_info *finfo;
		int arg_cnt;

		if (!btf_is_func(t))
			continue;

		func_name = btf__str_by_offset(att->vmlinux_btf, t->name_off);
		ksym = ksyms__get_symbol(att->ksyms, func_name);
		if (!ksym) {
			if (att->verbose)
				printf("Function '%s' not found in /proc/kallsyms! Skipping.\n", func_name);
			func_skip++;
			continue;
		}

		/* any deny glob forces skipping a function */
		for (j = 0; j < att->deny_glob_cnt; j++) {
			if (!glob_matches(att->deny_globs[j].glob, func_name))
				continue;
			att->deny_globs[j].matches++;
			if (att->debug_extra)
				printf("Function '%s' is denied by '%s' glob.\n",
				       func_name, att->deny_globs[j].glob);
			goto skip;
		}
		/* if any allow glob is specified, function has to match one of them */
		if (att->allow_glob_cnt) {
			for (j = 0; j < att->allow_glob_cnt; j++) {
				if (!glob_matches(att->allow_globs[j].glob, func_name))
					continue;
				att->allow_globs[j].matches++;
				if (att->debug_extra)
					printf("Function '%s' is allowed by '%s' glob.\n",
					       func_name, att->allow_globs[j].glob);
				goto proceed;
			}
			if (att->debug_extra)
				printf("Function '%s' doesn't match any allow glob, skipping.\n", func_name);
skip:
			func_skip++;
			continue;
		}

proceed:
		if (!is_kprobe_ok(att, func_name)) {
			if (att->debug_extra)
				printf("Function '%s' is not attachable kprobe, skipping.\n", func_name);
			func_skip++;
			continue;
		}
		if (!is_func_type_ok(att->vmlinux_btf, t)) {
			if (att->debug)
				printf("Function '%s' has prototype incompatible with fentry/fexit, skipping.\n", func_name);
			func_skip++;
			continue;
		}
		if (att->max_func_cnt && att->func_cnt >= att->max_func_cnt) {
			if (att->verbose)
				printf("Maximum allowed number of functions (%d) reached, skipping the rest.\n",
				       att->max_func_cnt);
			break;
		}

		if (att->func_filter && !att->func_filter(att, att->vmlinux_btf, i, func_name, att->func_cnt)) {
			if (att->debug)
				printf("Function '%s' skipped due to custom filter function.\n", func_name);
			func_skip++;
			continue;
		}

		arg_cnt = func_arg_cnt(att->vmlinux_btf, i);

		tmp = realloc(att->func_infos, (att->func_cnt + 1) * sizeof(*att->func_infos));
		if (!tmp)
			return -ENOMEM;
		att->func_infos = tmp;

		finfo = &att->func_infos[att->func_cnt];
		memset(finfo, 0, sizeof(*finfo));

		finfo->addr = ksym->addr;
		finfo->name = ksym->name;
		finfo->arg_cnt = arg_cnt;
		finfo->btf_id = i;

		att->func_info_cnts[arg_cnt]++;
		if (!att->func_info_id_for_arg_cnt[arg_cnt])
			att->func_info_id_for_arg_cnt[arg_cnt] = att->func_cnt;

		att->func_cnt++;

		if (att->debug_extra)
			printf("Found function '%s' at address 0x%lx...\n", func_name, ksym->addr);
	}

	if (att->func_cnt == 0) {
		fprintf(stderr, "No matching functions found.\n");
		return -ENOENT;
	}

	for (i = 0; i <= MAX_FUNC_ARG_CNT; i++) {
		struct mass_attacher_func_info *finfo;

		if (att->func_info_cnts[i]) {
			finfo = &att->func_infos[att->func_info_id_for_arg_cnt[i]];
			bpf_program__set_attach_target(att->fentries[i], 0, finfo->name);
			bpf_program__set_attach_target(att->fexits[i], 0, finfo->name);
			bpf_program__set_prep(att->fentries[i], 1, hijack_prog);
			bpf_program__set_prep(att->fexits[i], 1, hijack_prog);
			
			if (att->debug)
				printf("Found total %d functions with %d arguments.\n", att->func_info_cnts[i], i);
		} else {
			bpf_program__set_autoload(att->fentries[i], false);
			bpf_program__set_autoload(att->fexits[i], false);
		}
	}

	if (att->verbose) {
		printf("Found %d attachable functions in total.\n", att->func_cnt);
		printf("Skipped %d functions in total.\n", func_skip);

		if (att->debug) {
			for (i = 0; i < att->deny_glob_cnt; i++) {
				printf("Deny glob '%s' matched %d functions.\n",
				       att->deny_globs[i].glob, att->deny_globs[i].matches);
			}
			for (i = 0; i < att->allow_glob_cnt; i++) {
				printf("Allow glob '%s' matched %d functions.\n",
				       att->allow_globs[i].glob, att->allow_globs[i].matches);
			}
		}
	}

	bpf_map__set_max_entries(att->skel->maps.ip_to_id, att->func_cnt);

	return 0;
}

static int bump_rlimit(int resource, rlim_t max)
{
	struct rlimit rlim_new = {
		.rlim_cur	= max,
		.rlim_max	= max,
	};

	if (setrlimit(resource, &rlim_new))
		return -errno;

	return 0;
}

static int str_cmp(const void *a, const void *b)
{
	const char * const *s1 = a, * const *s2 = b;

	return strcmp(*s1, *s2);
}

static int load_available_kprobes(struct mass_attacher *att)
{
	static char buf[512];
	static char buf2[512];
	const char *fname = "/sys/kernel/tracing/available_filter_functions";
	int cnt, err;
	void *tmp, *s;
	FILE *f;

	f = fopen(fname, "r");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open %s: %d\n", fname, err);
		return err;
	}

	while ((cnt = fscanf(f, "%s%[^\n]\n", buf, buf2)) == 1) {
		tmp = realloc(att->kprobes, (att->kprobe_cnt + 1) * sizeof(*att->kprobes));
		if (!tmp)
			return -ENOMEM;
		att->kprobes = tmp;

		s = strdup(buf);
		att->kprobes[att->kprobe_cnt++] = s;
		if (!s)
			return -ENOMEM;
	}

	qsort(att->kprobes, att->kprobe_cnt, sizeof(char *), str_cmp);

	if (att->verbose)
		printf("Discovered %d available kprobes!\n", att->kprobe_cnt);

	return 0;
}

static int prog_arg_cnt(const struct mass_attacher *att, const struct bpf_program *p)
{
	int i;

	for (i = 0; i <= MAX_FUNC_ARG_CNT; i++) {
		if (att->fentries[i] == p || att->fexits[i] == p)
			return i;
	}

	return -1;
}

static int hijack_prog(struct bpf_program *prog, int n,
		       struct bpf_insn *insns, int insns_cnt,
		       struct bpf_prog_prep_result *res)
{
	struct mass_attacher *att = cur_attacher;
	struct bpf_insn **insns_ptr;
	size_t *insn_cnt_ptr;
	int arg_cnt;

	arg_cnt = prog_arg_cnt(att, prog);

	if (strncmp(bpf_program__name(prog), "fexit", sizeof("fexit") - 1) == 0) {
		insn_cnt_ptr = &att->fexits_insn_cnts[arg_cnt];
		insns_ptr = &att->fexits_insns[arg_cnt];
	} else {
		insn_cnt_ptr = &att->fentries_insn_cnts[arg_cnt];
		insns_ptr = &att->fentries_insns[arg_cnt];
	}

	*insns_ptr = malloc(sizeof(*insns) * insns_cnt);
	memcpy(*insns_ptr, insns, sizeof(*insns) * insns_cnt);
	*insn_cnt_ptr = insns_cnt;

	/* By not setting res->new_insn_ptr and res->new_insns_cnt we are
	 * preventing unnecessary load of the "prototype" BPF program.
	 * But we do load those programs in debug mode to use libbpf's logic
	 * of showing BPF verifier log, which is useful to debug verification
	 * errors.
	 */
	if (att->debug) {
		res->new_insn_ptr = insns;
		res->new_insn_cnt = insns_cnt;
	}

	return 0;
}


static int clone_prog(const struct bpf_program *prog,
		      struct bpf_insn *insns, size_t insn_cnt, int attach_btf_id);

int mass_attacher__load(struct mass_attacher *att)
{
	int err, i, map_fd;

	/* we can't pass extra context to hijack_progs, so we set thread-local
	 * cur_attacher variable temporarily for the duration of skeleton's
	 * load phase
	 */
	cur_attacher = att;
	/* Load & verify BPF programs */
	err = SKEL_LOAD(att->skel);
	cur_attacher = NULL;

	if (err) {
		fprintf(stderr, "Failed to load and verify BPF skeleton\n");
		return err;
	}

	if (att->debug)
		printf("Preparing %d BPF program copies...\n", att->func_cnt * 2);

	for (i = 0; i < att->func_cnt; i++) {
		struct mass_attacher_func_info *finfo = &att->func_infos[i];
		const char *func_name = att->func_infos[i].name;
		long func_addr = att->func_infos[i].addr;

		map_fd = bpf_map__fd(att->skel->maps.ip_to_id);
		err = bpf_map_update_elem(map_fd, &func_addr, &i, 0);
		if (err) {
			err = -errno;
			fprintf(stderr, "Failed to add 0x%lx -> '%s' lookup entry to BPF map: %d\n",
				func_addr, func_name, err);
			return err;
		}

		err = clone_prog(att->fentries[finfo->arg_cnt],
				 att->fentries_insns[finfo->arg_cnt],
				 att->fentries_insn_cnts[finfo->arg_cnt],
				 finfo->btf_id);
		if (err < 0) {
			fprintf(stderr, "Failed to clone FENTRY BPF program for function '%s': %d\n", func_name, err);
			return err;
		}
		finfo->fentry_prog_fd = err;

		err = clone_prog(att->fexits[finfo->arg_cnt],
				 att->fexits_insns[finfo->arg_cnt],
				 att->fexits_insn_cnts[finfo->arg_cnt],
				 finfo->btf_id);
		if (err < 0) {
			fprintf(stderr, "Failed to clone FEXIT BPF program for function '%s': %d\n", func_name, err);
			return err;
		}
		finfo->fexit_prog_fd = err;
	}
	return 0;
}

static int clone_prog(const struct bpf_program *prog,
		      struct bpf_insn *insns, size_t insn_cnt, int attach_btf_id)
{
	struct bpf_load_program_attr attr;
	int fd;

	memset(&attr, 0, sizeof(attr));

	attr.prog_type = bpf_program__get_type(prog);
	attr.expected_attach_type = bpf_program__get_expected_attach_type(prog);
	attr.name = bpf_program__name(prog);
	attr.insns = insns;
	attr.insns_cnt = insn_cnt;
	attr.license = "Dual BSD/GPL";
	attr.attach_btf_id = attach_btf_id;

	fd = bpf_load_program_xattr(&attr, NULL, 0);
	if (fd < 0)
		return -errno;

	return fd;
}

int mass_attacher__attach(struct mass_attacher *att)
{
	int i, err, prog_fd;

	for (i = 0; i < att->func_cnt; i++) {
		if (att->debug)
			printf("Attaching function '%s' (#%d at addr %lx)...\n",
			       att->func_infos[i].name, i + 1, att->func_infos[i].addr);

		prog_fd = att->func_infos[i].fentry_prog_fd;
		err = bpf_raw_tracepoint_open(NULL, prog_fd);
		if (err < 0) {
			fprintf(stderr, "Failed to attach FENTRY prog (fd %d) for func #%d (%s), skipping: %d\n",
				prog_fd, i + 1, att->func_infos[i].name, -errno);
		}

		prog_fd = att->func_infos[i].fexit_prog_fd;
		err = bpf_raw_tracepoint_open(NULL, prog_fd);
		if (err < 0) {
			fprintf(stderr, "Failed to attach FEXIT prog (fd %d) for func #%d (%s), skipping: %d\n",
				prog_fd, i + 1, att->func_infos[i].name, -errno);
		}
	}

	if (att->verbose)
		printf("Total %d BPF programs attached successfully!\n", 2 * att->func_cnt);

	return 0;
}

void mass_attacher__activate(struct mass_attacher *att)
{
	att->skel->bss->ready = true;
}

struct SKEL_NAME *mass_attacher__skeleton(const struct mass_attacher *att)
{
	return att->skel;
}

const struct btf *mass_attacher__btf(const struct mass_attacher *att)
{
	return att->vmlinux_btf;
}

size_t mass_attacher__func_cnt(const struct mass_attacher *att)
{
	return att->func_cnt;
}

const struct mass_attacher_func_info *mass_attacher__func(const struct mass_attacher *att, int id)
{
	if (id < 0 || id >= att->func_cnt)
		return NULL;
	return &att->func_infos[id];
}

static bool is_kprobe_ok(const struct mass_attacher *att, const char *name)
{
	void *r;

	/*
	if (strcmp(name, "__x64_sys_getpgid") == 0) 
		r = NULL;
	*/
	r = bsearch(&name, att->kprobes, att->kprobe_cnt, sizeof(void *), str_cmp);

	return r != NULL;
}

static int func_arg_cnt(const struct btf *btf, int id)
{
	const struct btf_type *t;

	t = btf__type_by_id(btf, id);
	t = btf__type_by_id(btf, t->type);
	return btf_vlen(t);
}

static bool is_arg_type_ok(const struct btf *btf, const struct btf_type *t)
{
	while (btf_is_mod(t) || btf_is_typedef(t))
		t = btf__type_by_id(btf, t->type);
	if (!btf_is_int(t) && !btf_is_ptr(t) && !btf_is_enum(t))
		return false;
	return true;
}

static bool is_ret_type_ok(const struct btf *btf, const struct btf_type *t)
{
	while (btf_is_mod(t) || btf_is_typedef(t))
		t = btf__type_by_id(btf, t->type);

	if (btf_is_int(t) || btf_is_enum(t))
		return true;

	/* non-pointer types are rejected */
	if (!btf_is_ptr(t))
		return false;

	/* pointer to void is fine */
	if (t->type == 0) 
		return true;

	/* only pointer to struct/union is allowed */
	t = btf__type_by_id(btf, t->type);
	if (!btf_is_composite(t))
		return false;

	return true;
}

static bool is_func_type_ok(const struct btf *btf, const struct btf_type *t)
{
	const struct btf_param *p;
	int i;

	t = btf__type_by_id(btf, t->type);
	if (btf_vlen(t) > MAX_FUNC_ARG_CNT)
		return false;

	/* IGNORE VOID FUNCTIONS, THIS SHOULDN'T BE DONE IN GENERAL!!! */
	if (!t->type)
		return false;

	if (t->type && !is_ret_type_ok(btf, btf__type_by_id(btf, t->type)))
		return false;

	for (i = 0; i < btf_vlen(t); i++) {
		p = btf_params(t) + i;

		/* vararg not supported */
		if (!p->type)
			return false;

		if (!is_arg_type_ok(btf, btf__type_by_id(btf, p->type)))
			return false;
	}

	return true;
}

bool glob_matches(const char *glob, const char *s)
{
	int n = strlen(glob);

	if (n == 1 && glob[0] == '*')
		return true;

	if (glob[0] == '*' && glob[n - 1] == '*') {
		const char *subs;
		/* substring match */

		/* this is hacky, but we don't want to allocate for no good reason */
		((char *)glob)[n - 1] = '\0';
		subs = strstr(s, glob + 1);
		((char *)glob)[n - 1] = '*';

		return subs != NULL;
	} else if (glob[0] == '*') {
		size_t nn = strlen(s);
		/* suffix match */

		/* too short for a given suffix */
		if (nn < n - 1)
			return false;

		return strcmp(s + nn - (n - 1), glob + 1) == 0;
	} else if (glob[n - 1] == '*') {
		/* prefix match */
		return strncmp(s, glob, n - 1) == 0;
	} else {
		/* exact match */
		return strcmp(glob, s) == 0;
	}
}

