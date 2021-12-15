#include "posix.h"

#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "coerce.h"
#include "functions/common.h"
#include "functions/default.h"
#include "functions/default/build_target.h"
#include "functions/default/configure_file.h"
#include "functions/default/custom_target.h"
#include "functions/default/dependency.h"
#include "functions/default/options.h"
#include "functions/default/setup.h"
#include "functions/environment.h"
#include "functions/modules.h"
#include "functions/string.h"
#include "guess.h"
#include "lang/interpreter.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"

static bool
s_to_lang(struct workspace *wk, uint32_t err_node, obj lang, enum compiler_language *l)
{
	if (!s_to_compiler_language(get_cstr(wk, lang), l)) {
		interp_error(wk, err_node, "unknown language '%s'", get_cstr(wk, lang));
		return false;
	}

	return true;
}

static bool
project_add_language(struct workspace *wk, uint32_t err_node, obj str, enum requirement_type req, bool *found)
{
	if (req == requirement_skip) {
		return true;
	}

	uint32_t comp_id;
	enum compiler_language l;
	if (!s_to_lang(wk, err_node, str, &l)) {
		return false;
	}

	obj res;
	if (obj_dict_geti(wk, current_project(wk)->compilers, l, &res)) {
		interp_error(wk, err_node, "language '%s' has already been added", get_cstr(wk, str));
		return false;
	}

	if (!compiler_detect(wk, &comp_id, l)) {
		if (req == requirement_required) {
			interp_error(wk, err_node, "unable to detect %s compiler", get_cstr(wk, str));
			return false;
		} else {
			return true;
		}
	}

	obj_dict_seti(wk, current_project(wk)->compilers, l, comp_id);
	*found = true;
	return true;
}

struct project_add_language_iter_ctx {
	uint32_t err_node;
	enum requirement_type req;
	bool missing;
};

static enum iteration_result
project_add_language_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct project_add_language_iter_ctx *ctx = _ctx;

	bool found = false;
	if (!project_add_language(wk, ctx->err_node, val, ctx->req, &found)) {
		return ir_err;
	}

	if (!found) {
		ctx->missing = true;
	}

	return ir_cont;
}

static bool
func_project(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_default_options,
		kw_license,
		kw_meson_version,
		kw_subproject_dir,
		kw_version
	};
	struct args_kw akw[] = {
		[kw_default_options] = { "default_options", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_license] = { "license", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_meson_version] = { "meson_version", obj_string },
		[kw_subproject_dir] = { "subproject_dir", obj_string },
		[kw_version] = { "version", obj_any },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	current_project(wk)->cfg.name = an[0].val;

	if (!obj_array_foreach_flat(wk, an[1].val,
		&(struct project_add_language_iter_ctx) {
		.err_node = an[1].node,
		.req = requirement_required,
	}, project_add_language_iter)) {
		return false;
	}

	current_project(wk)->cfg.license = akw[kw_license].val;

	if (akw[kw_version].set) {
		struct obj *ver = get_obj(wk, akw[kw_version].val);
		if (ver->type == obj_array) {
			if (ver->dat.arr.len != 1) {
				goto version_type_error;
				// type error
				return false;
			}

			obj e;
			obj_array_index(wk, akw[kw_version].val, 0, &e);
			ver = get_obj(wk, e);
		}

		if (ver->type == obj_string) {
			current_project(wk)->cfg.version = akw[kw_version].val;
		} else if (ver->type == obj_file) {
			struct source ver_src = { 0 };
			if (!fs_read_entire_file(get_cstr(wk, ver->dat.file), &ver_src)) {
				interp_error(wk, akw[kw_version].node, "failed to read version file");
				return false;
			}

			const char *str_ver = ver_src.src;
			uint32_t i;
			for (i = 0; ver_src.src[i]; ++i) {
				if (ver_src.src[i] == '\n') {
					if (ver_src.src[i + 1]) {
						interp_error(wk, akw[kw_version].node, "version file is more than one line long");
						return false;
					}
					break;
				}
			}

			current_project(wk)->cfg.version = make_strn(wk, str_ver, i);

			fs_source_destroy(&ver_src);
		} else {
version_type_error:
			interp_error(wk, akw[kw_version].node,
				"invalid type for version: '%s'",
				obj_type_to_s(get_obj(wk, akw[kw_version].val)->type));
			return false;
		}
	} else {
		current_project(wk)->cfg.version = make_str(wk, "unknown");
	}

	if (akw[kw_default_options].set) {
		if (!parse_and_set_default_options(wk, akw[kw_default_options].node, akw[kw_default_options].val, 0)) {
			return false;
		}
	}

	LOG_I("configuring '%s', version: %s",
		get_cstr(wk, current_project(wk)->cfg.name),
		get_cstr(wk, current_project(wk)->cfg.version)
		);
	return true;
}

struct add_arguments_ctx {
	uint32_t lang_node;
	uint32_t args_node;
	obj args_dict;
	obj args_to_add;
	obj arg_arr;
};

static enum iteration_result
add_arguments_language_iter(struct workspace *wk, void *_ctx, obj val_id)
{
	struct add_arguments_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->args_node, val_id, obj_string)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->arg_arr, val_id);

	return ir_cont;
}

static enum iteration_result
add_arguments_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct add_arguments_ctx *ctx = _ctx;
	enum compiler_language l;

	if (!s_to_lang(wk, ctx->lang_node, val, &l)) {
		return false;
	}

	obj arg_arr;
	if (!obj_dict_geti(wk, ctx->args_dict, l, &arg_arr)) {
		make_obj(wk, &arg_arr, obj_array);
		obj_dict_seti(wk, ctx->args_dict, l, arg_arr);
	}

	ctx->arg_arr = arg_arr;

	if (!obj_array_foreach_flat(wk, ctx->args_to_add, ctx, add_arguments_language_iter)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
add_arguments_common(struct workspace *wk, uint32_t args_node, obj args_dict, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_language,
		kw_native, // ignored
	};
	struct args_kw akw[] = {
		[kw_language] = { "language", ARG_TYPE_ARRAY_OF | obj_string, .required = true },
		[kw_native] = { "native", obj_bool },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	struct add_arguments_ctx ctx = {
		.lang_node = akw[kw_language].node,
		.args_node = an[0].node,
		.args_dict = args_dict,
		.args_to_add = an[0].val,
	};
	return obj_array_foreach(wk, akw[kw_language].val, &ctx, add_arguments_iter);
}

static bool
func_add_project_arguments(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return add_arguments_common(wk, args_node, current_project(wk)->args, res);
}

static bool
func_add_global_arguments(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	if (wk->cur_project != 0) {
		interp_error(wk, args_node, "add_global_arguments cannot be called from a subproject");
		return false;
	}

	return add_arguments_common(wk, args_node, wk->global_args, res);
}

static bool
func_add_project_link_arguments(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return add_arguments_common(wk, args_node, current_project(wk)->link_args, res);
}

static bool
func_add_global_link_arguments(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	if (wk->cur_project != 0) {
		interp_error(wk, args_node, "add_global_link_arguments cannot be called from a subproject");
		return false;
	}

	return add_arguments_common(wk, args_node, wk->global_link_args, res);
}

static bool
func_add_languages(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required", obj_any },
		[kw_native] = { "version", obj_bool },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	struct project_add_language_iter_ctx ctx = {
		.err_node = an[0].node,
	};

	if (!coerce_requirement(wk, &akw[kw_required], &ctx.req)) {
		return false;
	}

	if (!obj_array_foreach(wk, an[0].val, &ctx, project_add_language_iter)) {
		return false;
	}

	make_obj(wk, res, obj_bool)->dat.boolean = !ctx.missing;
	return true;
}

static bool
func_files(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	return coerce_files(wk, an[0].node, an[0].val, res);
}

struct find_program_iter_ctx {
	bool found;
	uint32_t node, version_node;
	obj version;
	obj dirs;
	obj *res;
};

struct find_program_custom_dir_ctx {
	const char *prog;
	char buf[PATH_MAX];
	bool found;
};

static enum iteration_result
find_program_custom_dir_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct find_program_custom_dir_ctx *ctx = _ctx;

	assert(get_obj(wk, val)->type == obj_file);

	if (!path_join(ctx->buf, PATH_MAX, get_cstr(wk, get_obj(wk, val)->dat.file), ctx->prog)) {
		return ir_err;
	}

	if (fs_file_exists(ctx->buf)) {
		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static enum iteration_result
find_program_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct find_program_iter_ctx *ctx = _ctx;
	const char *str;
	obj ver = 0;
	struct run_cmd_ctx cmd_ctx = { 0 };

	struct obj *v = get_obj(wk, val);
	switch (v->type) {
	case obj_file:
		str = get_cstr(wk, get_obj(wk, val)->dat.file);
		break;
	case obj_string:
		str = get_cstr(wk, val);
		break;
	default:
		interp_error(wk, ctx->node, "expected string or file, got %o", val);
		return ir_err;
	}

	const char *path;

	struct find_program_custom_dir_ctx dir_ctx = {
		.prog = str,
	};

	/* TODO: 1. Program overrides set via meson.override_find_program() */
	/* TODO: 2. [provide] sections in subproject wrap files, if wrap_mode is set to forcefallback */
	/* TODO: 3. [binaries] section in your machine files */

	/* 4. Directories provided using the dirs: kwarg */
	if (ctx->dirs) {
		if (!obj_array_foreach(wk, ctx->dirs, &dir_ctx, find_program_custom_dir_iter)) {
			return false;
		} else if (dir_ctx.found) {
			path = dir_ctx.buf;
			goto found;
		}
	}

	/* 5. Project's source tree relative to the current subdir */
	/*       If you use the return value of configure_file(), the current subdir inside the build tree is used instead */
	if (!path_join(dir_ctx.buf, PATH_MAX, get_cstr(wk, current_project(wk)->cwd), str)) {
		return false;
	} else if (fs_file_exists(dir_ctx.buf)) {
		path = dir_ctx.buf;
		goto found;
	}

	/* 6. PATH environment variable */
	if (fs_find_cmd(str, &path)) {
		goto found;
	}

	/* TODO: 7. [provide] sections in subproject wrap files, if wrap_mode is set to anything other than nofallback */

	return ir_cont;
found:
	if (ctx->version) {
		if (run_cmd(&cmd_ctx, path, (const char *[]){ (char *)path, "--version", 0 }, NULL)
		    && cmd_ctx.status == 0) {
			guess_version(wk, cmd_ctx.out.buf, &ver);
		}

		run_cmd_ctx_destroy(&cmd_ctx);

		if (!ver) {
			return ir_cont; // no version to check against
		}

		struct version v = { 0 };
		if (string_to_version(wk, &v, get_str(wk, ver))) {
			bool comparison_result;
			if (!version_compare(wk, ctx->version_node, &v, ctx->version, &comparison_result)) {
				return ir_err;
			} else if (!comparison_result) {
				return ir_cont;
			}
		}
	}

	struct obj *external_program = make_obj(wk, ctx->res, obj_external_program);
	external_program->dat.external_program.found = true;
	external_program->dat.external_program.full_path = make_str(wk, path);
	external_program->dat.external_program.ver = ver;

	ctx->found = true;
	return ir_done;
}

static bool
func_find_program(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native,
		kw_disabler,
		kw_dirs,
		kw_version,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required" },
		[kw_native] = { "native", obj_bool },
		[kw_disabler] = { "disabler", obj_bool },
		[kw_dirs] = { "dirs", ARG_TYPE_ARRAY_OF | obj_any  },
		[kw_version] = { "version", ARG_TYPE_ARRAY_OF | obj_string },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj dirs = 0;
	if (akw[kw_dirs].set) {
		if (!coerce_dirs(wk, akw[kw_dirs].node, akw[kw_dirs].val, &dirs)) {
			return false;
		}
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	if (requirement == requirement_skip) {
		make_obj(wk, res, obj_external_program)->dat.external_program.found = false;
		return true;
	}

	struct find_program_iter_ctx ctx = {
		.node = an[0].node,
		.version = akw[kw_version].val,
		.dirs = dirs,
		.res = res,
	};
	obj_array_foreach_flat(wk, an[0].val, &ctx, find_program_iter);

	if (!ctx.found) {
		if (requirement == requirement_required) {
			interp_error(wk, an[0].node, "program not found");
			return false;
		}

		if (akw[kw_disabler].set && get_obj(wk, akw[kw_disabler].val)->dat.boolean) {
			*res = disabler_id;
		} else {
			make_obj(wk, res, obj_external_program)->dat.external_program.found = false;
		}
	}

	return true;
}

static bool
func_include_directories(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_is_system,
	};
	struct args_kw akw[] = {
		[kw_is_system] = { "is_system", obj_bool },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	bool is_system = akw[kw_is_system].set
		? get_obj(wk, akw[kw_is_system].val)->dat.boolean
		: false;

	if (!coerce_include_dirs(wk, an[0].node, an[0].val, is_system, res)) {
		return false;
	}

	return true;
}

static enum iteration_result
mangle_generator_output(struct workspace *wk, void *_ctx, obj val)
{
	struct obj *s = get_obj(wk, val), *gen = _ctx;
	assert(s->type == obj_string);

	obj_array_push(wk, gen->dat.generator.output,
		make_strf(wk, "muon-generated_%s", get_cstr(wk, val)));
	return ir_cont;
}

static bool
func_generator(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_any }, ARG_TYPE_NULL };
	enum kwargs {
		kw_output,
		kw_arguments,
		kw_capture,
		kw_depfile,
	};
	struct args_kw akw[] = {
		[kw_output]      = { "output", ARG_TYPE_ARRAY_OF | obj_string, .required = true },
		[kw_arguments]   = { "arguments", obj_array, .required = true },
		[kw_capture]     = { "capture", obj_bool },
		[kw_depfile]     = { "depfile", obj_string },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj command, args;
	make_obj(wk, &command, obj_array);

	if (akw[kw_arguments].set) {
		obj_array_dup(wk, akw[kw_arguments].val, &args);
	} else {
		make_obj(wk, &args, obj_array);
	}

	obj_array_push(wk, command, an[0].val);
	obj_array_extend(wk, command, args);

	struct obj *gen = make_obj(wk, res, obj_generator);

	make_obj(wk, &gen->dat.generator.output, obj_array);
	obj_array_foreach(wk, akw[kw_output].val, gen, mangle_generator_output);

	gen->dat.generator.raw_command = command;
	gen->dat.generator.depfile = akw[kw_depfile].val;
	return true;
}

static bool
func_assert(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_bool }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, ao, NULL)) {
		return false;
	}

	*res = 0;

	if (!get_obj(wk, an[0].val)->dat.boolean) {
		if (ao[0].set) {
			LOG_E("%s", get_cstr(wk, ao[0].val));
		}
		return false;
	}

	return true;
}

static bool
func_error(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	LOG_E("%s", get_cstr(wk, an[0].val));
	*res = 0;

	return false;
}

static bool
func_warning(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	LOG_W("%s", get_cstr(wk, an[0].val));
	*res = 0;

	return true;
}

static bool
func_message(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	LOG_I("%s", get_cstr(wk, an[0].val));
	*res = 0;

	return true;
}

#define BASE_PATH_MAX (PATH_MAX / 2)

static bool
func_subproject(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_default_options,
		kw_required,
	};
	struct args_kw akw[] = {
		[kw_default_options] = { "default_options", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_required] = { "required", obj_any },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	// don't re-evaluate the same subproject
	if (obj_dict_index(wk, wk->subprojects, an[0].val, res)) {
		return true;
	}

	enum requirement_type req;
	if (!coerce_requirement(wk, &akw[kw_required], &req)) {
		return false;
	} else if (req == requirement_skip) {
		make_obj(wk, res, obj_subproject);
		return true;
	}

	const char *subproj_name = get_cstr(wk, an[0].val);
	char buf[PATH_MAX], cwd[PATH_MAX], build_dir[PATH_MAX];

	if (!path_join(buf, PATH_MAX, get_cstr(wk, current_project(wk)->source_root), "subprojects")) {
		return false;
	} else if (!path_join(cwd, PATH_MAX, buf, subproj_name)) {
		return false;
	}

	if (!path_join(buf, PATH_MAX, wk->build_root, "subprojects")) {
		return false;
	} else if (!path_join(build_dir, PATH_MAX, buf, subproj_name)) {
		return false;
	}

	if (akw[kw_default_options].set) {
		if (!parse_and_set_default_options(wk, akw[kw_default_options].node,
			akw[kw_default_options].val, an[0].val)) {
			return false;
		}
	}

	uint32_t subproject_id;
	bool found;
	if (!eval_project(wk, subproj_name, cwd, build_dir, &subproject_id,
		req == requirement_required, &found)) {
		return false;
	}

	if (!found) {
		make_obj(wk, res, obj_subproject);
		return true;
	}

	struct obj *sub = make_obj(wk, res, obj_subproject);
	sub->dat.subproj.id = subproject_id;
	sub->dat.subproj.found = true;

	obj_dict_set(wk, wk->subprojects, an[0].val, *res);

	return true;
}

static bool
func_run_command(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_check,
		kw_env,
	};
	struct args_kw akw[] = {
		[kw_check] = { "check", obj_bool },
		[kw_env] = { "env", obj_any },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj cmd;
	const char *argv[MAX_ARGS];
	char *const *envp = NULL;

	{
		obj args;
		if (!arr_to_args(wk, an[0].val, &args)) {
			return false;
		}

		int64_t i = 0;
		if (!boundscheck(wk, an[0].node, args, &i)) {
			return false;
		}
		obj_array_index(wk, args, 0, &cmd);

		if (!join_args_argv(wk, argv, MAX_ARGS, args)) {
			return false;
		}
	}

	if (!env_to_envp(wk, akw[kw_env].node, &envp, akw[kw_env].val, env_to_envp_flag_subdir)) {
		return false;
	}

	bool ret = false;
	struct run_cmd_ctx cmd_ctx = { 0 };

	if (!run_cmd(&cmd_ctx, get_cstr(wk, cmd), argv, envp)) {
		interp_error(wk, an[0].node, "error: %s", cmd_ctx.err_msg);
		goto ret;
	}

	if (akw[kw_check].set && get_obj(wk, akw[kw_check].val)->dat.boolean
	    && cmd_ctx.status != 0) {
		interp_error(wk, an[0].node, "command failed: '%s'", cmd_ctx.err.buf);
		return false;

	}

	struct obj *run_result = make_obj(wk, res, obj_run_result);
	run_result->dat.run_result.status = cmd_ctx.status;
	run_result->dat.run_result.out = make_strn(wk, cmd_ctx.out.buf, cmd_ctx.out.len);
	run_result->dat.run_result.err = make_strn(wk, cmd_ctx.err.buf, cmd_ctx.err.len);

	ret = true;
ret:
	run_cmd_ctx_destroy(&cmd_ctx);
	return ret;
}

static bool
func_run_target(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_command,
		kw_depends,
		kw_env,
	};
	struct args_kw akw[] = {
		[kw_command] = { "command", obj_any },
		[kw_depends] = { "depends", obj_any },
		[kw_env] = { "env", obj_any },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	LOG_W("TODO: run_target()");
	return true;
}

static bool
func_subdir(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	char src[PATH_MAX], cwd[PATH_MAX], build_dir[PATH_MAX];

	obj old_cwd = current_project(wk)->cwd;
	obj old_build_dir = current_project(wk)->build_dir;

	if (!path_join(cwd, PATH_MAX, get_cstr(wk, old_cwd), get_cstr(wk, an[0].val))) {
		return false;
	} else if (!path_join(build_dir, PATH_MAX, get_cstr(wk, old_build_dir), get_cstr(wk, an[0].val))) {
		return false;
	} else if (!path_join(src, PATH_MAX, cwd, "meson.build")) {
		return false;
	}

	current_project(wk)->cwd = make_str(wk, cwd);
	current_project(wk)->build_dir = make_str(wk, build_dir);

	bool ret = eval_project_file(wk, src);
	current_project(wk)->cwd = old_cwd;
	current_project(wk)->build_dir = old_build_dir;

	return ret;
}

static bool
func_configuration_data(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_dict }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, NULL, ao, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_configuration_data);

	if (ao[0].set) {
		get_obj(wk, *res)->dat.configuration_data.dict = ao[0].val;
	} else {
		obj dict;
		make_obj(wk, &dict, obj_dict);
		get_obj(wk, *res)->dat.configuration_data.dict = dict;
	}

	return true;
}

static bool
func_install_subdir(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_install_tag,
		kw_exclude_directories,
		kw_exclude_files,
		kw_strip_directory,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", ARG_TYPE_ARRAY_OF | obj_any },
		[kw_install_tag] = { "install_tag", obj_string },
		[kw_exclude_directories] = { "exclude_directories", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_exclude_files] = { "exclude_files", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_strip_directory] = { "strip_directory", obj_bool },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	L("TODO: installation");

	return true;
}

static bool
func_install_man(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_locale,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", ARG_TYPE_ARRAY_OF | obj_any },
		[kw_locale] = { "locale", obj_string },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	L("TODO: installation");

	return true;
}

struct install_data_rename_ctx {
	obj rename;
	obj mode;
	obj dest;
	uint32_t i;
	uint32_t node;
};

static enum iteration_result
install_data_rename_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct install_data_rename_ctx *ctx = _ctx;

	struct obj *v = get_obj(wk, val);
	assert(v->type == obj_file);

	obj src = v->dat.file;
	obj dest;

	obj rename;
	obj_array_index(wk, ctx->rename, ctx->i, &rename);

	char d[PATH_MAX];
	if (!path_join(d, PATH_MAX, get_cstr(wk, ctx->dest), get_cstr(wk, rename))) {
		return false;
	}

	dest = make_str(wk, d);

	push_install_target(wk, src, dest, ctx->mode);

	++ctx->i;
	return ir_cont;
}

static bool
func_install_data(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_install_tag,
		kw_rename,
		kw_sources,
	};

	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", ARG_TYPE_ARRAY_OF | obj_any },
		[kw_install_tag] = { "install_tag", obj_string }, // TODO
		[kw_rename] = { "rename", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_sources] = { "sources", ARG_TYPE_ARRAY_OF | obj_any },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj install_dir;
	if (akw[kw_install_dir].set) {
		install_dir = akw[kw_install_dir].val;
	} else {
		obj install_dir_base;
		get_option(wk, current_project(wk), "datadir", &install_dir_base);

		char buf[PATH_MAX];
		if (!path_join(buf, PATH_MAX, get_cstr(wk, install_dir_base), get_cstr(wk, current_project(wk)->cfg.name))) {
			return false;
		}

		install_dir = make_str(wk, buf);

	}

	obj datafiles;
	obj sources;
	if (!coerce_files(wk, an[0].node, an[0].val, &datafiles)) {
		return false;
	}
	if (akw[kw_sources].set) {
		if (!coerce_files(wk, akw[kw_sources].node, akw[kw_sources].val, &sources)) {
			return false;
		}
		obj_array_extend(wk, datafiles, sources);
	}

	if (akw[kw_rename].set) {
		if (get_obj(wk, akw[kw_rename].val)->dat.arr.len !=
		    get_obj(wk, datafiles)->dat.arr.len) {
			interp_error(wk, akw[kw_rename].node, "number of elements in rename != number if sources");
			return false;
		}

		struct install_data_rename_ctx ctx = {
			.node = an[0].node,
			.mode = akw[kw_install_mode].val,
			.rename = akw[kw_rename].val,
			.dest = install_dir,
		};

		return obj_array_foreach(wk, datafiles, &ctx, install_data_rename_iter);
	} else {
		return push_install_targets(wk, datafiles, install_dir, akw[kw_install_mode].val);
	}
}

static bool
func_install_headers(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_subdir,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", ARG_TYPE_ARRAY_OF | obj_any },
		[kw_subdir] = { "subdir", obj_string },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj install_dir_base;
	if (akw[kw_install_dir].set) {
		install_dir_base = akw[kw_install_dir].val;
	} else {
		get_option(wk, current_project(wk), "includedir", &install_dir_base);
	}

	obj install_dir;
	if (akw[kw_subdir].set) {
		char buf[PATH_MAX];
		if (!path_join(buf, PATH_MAX, get_cstr(wk, install_dir_base), get_cstr(wk, akw[kw_subdir].val))) {
			return false;
		}

		install_dir = make_str(wk, buf);
	} else {
		install_dir = install_dir_base;
	}

	obj headers;
	if (!coerce_files(wk, an[0].node, an[0].val, &headers)) {
		return false;
	}

	return push_install_targets(wk, headers, install_dir, akw[kw_install_mode].val);
}

static bool
func_test(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { obj_string }, { obj_any }, ARG_TYPE_NULL };
	enum kwargs {
		kw_args,
		kw_workdir,
		kw_depends,
		kw_should_fail,
		kw_env,
		kw_suite,
		kw_priority, // TODO
		kw_timeout, // TODO
		kw_protocol, // TODO
	};
	struct args_kw akw[] = {
		[kw_args] = { "args", ARG_TYPE_ARRAY_OF | obj_any, },
		[kw_workdir] = { "workdir", obj_string, },
		[kw_depends] = { "depends", obj_array, }, // TODO
		[kw_should_fail] = { "should_fail", obj_bool, },
		[kw_env] = { "env", obj_any, },
		[kw_suite] = { "suite", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_priority] = { "priority", obj_number, }, // TODO
		[kw_timeout] = { "timeout", obj_number, }, // TODO
		[kw_protocol] = { "protocol", obj_string, }, // TODO
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	obj exe;
	if (!coerce_executable(wk, an[1].node, an[1].val, &exe)) {
		return false;
	}

	obj args = 0;
	if (akw[kw_args].set) {
		if (!arr_to_args(wk, akw[kw_args].val, &args)) {
			return false;
		}
	}

	if (akw[kw_env].set) {
		char *const *envp;
		/* even though we won't use the result, do this type-checking
		 * here so you don't get type errors when running tests */
		if (!env_to_envp(wk, akw[kw_env].node, &envp, akw[kw_env].val, 0)) {
			return false;
		}
	}

	obj test;
	struct obj *t = make_obj(wk, &test, obj_test);
	t->dat.test.name = an[0].val;
	t->dat.test.exe = exe;
	t->dat.test.args = args;
	t->dat.test.env = akw[kw_env].val;
	t->dat.test.should_fail =
		akw[kw_should_fail].set
		&& get_obj(wk, akw[kw_should_fail].val)->dat.boolean;
	t->dat.test.suites = akw[kw_suite].val;
	t->dat.test.workdir = akw[kw_workdir].val;

	obj_array_push(wk, current_project(wk)->tests, test);
	return true;
}

struct join_paths_ctx {
	uint32_t node;
	char buf[PATH_MAX];
};

static enum iteration_result
join_paths_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct join_paths_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->node, val, obj_string)) {
		return ir_err;
	}

	char buf[PATH_MAX];
	strcpy(buf, ctx->buf);

	if (!path_join(ctx->buf, PATH_MAX, buf, get_cstr(wk, val))) {
		return ir_err;
	}

	return ir_cont;
}

static bool
func_join_paths(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB }, ARG_TYPE_NULL };

	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	struct join_paths_ctx ctx = {
		.node = args_node,
	};

	if (!obj_array_foreach_flat(wk, an[0].val, &ctx, join_paths_iter)) {
		return false;
	}

	*res = make_str(wk, ctx.buf);
	return true;
}

static bool
func_environment(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_dict }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, NULL, ao, NULL)) {
		return false;
	}

	struct obj *d = make_obj(wk, res, obj_environment);

	if (ao[0].set) {
		if (!typecheck_environment_dict(wk, ao[0].node, ao[0].val)) {
			return false;
		}
		d->dat.environment.env = ao[0].val;
	} else {
		make_obj(wk, &d->dat.environment.env, obj_dict);
	}

	return true;
}

static bool
func_import(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required", obj_any },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	struct obj *m = make_obj(wk, res, obj_module);
	m->dat.module.found = false;

	if (requirement == requirement_skip) {
		return true;
	}

	enum module mod;
	if (!module_lookup(get_cstr(wk, an[0].val), &mod)) {
		if (requirement == requirement_required) {
			interp_error(wk, an[0].node, "module not found");
			return false;
		} else {
			return true;
		}
	}

	m->dat.module.module = mod;
	m->dat.module.found = true;
	return true;
}

static bool
func_is_disabler(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_any }, ARG_TYPE_NULL };

	disabler_among_args_immunity = true;
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	make_obj(wk, res, obj_bool)->dat.boolean = an[0].val == disabler_id;
	return true;
}

static bool
func_disabler(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	*res = disabler_id;
	return true;
}

static bool
func_set_variable(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { obj_any }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	hash_set(&current_project(wk)->scope, get_cstr(wk, an[0].val), an[1].val);
	return true;
}

static bool
func_get_variable(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_any }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_any }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!interp_args(wk, args_node, an, ao, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	if (an[0].val == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, an[0].node, an[0].val, obj_string)) {
		return false;
	}

	if (!get_obj_id(wk, get_cstr(wk, an[0].val), res, wk->cur_project)) {
		if (ao[0].set) {
			*res = ao[0].val;
		} else {
			interp_error(wk, an[0].node, "undefined object");
			return false;
		}
	}

	return true;
}

static bool
func_is_variable(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	obj dont_care;
	make_obj(wk, res, obj_bool)->dat.boolean = get_obj_id(wk, get_cstr(wk, an[0].val), &dont_care, wk->cur_project);
	return true;
}

static bool
func_subdir_done(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	wk->subdir_done = true;
	return true;
}

static bool
func_summary(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_any }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_any }, ARG_TYPE_NULL };
	enum kwargs {
		kw_section,
		kw_bool_yn, // ignored
	};
	struct args_kw akw[] = {
		[kw_section] = { "section", obj_string, },
		[kw_bool_yn] = { "bool_yn", obj_bool, },
		0
	};
	if (!interp_args(wk, args_node, an, ao, akw)) {
		return false;
	}

	obj sec = akw[kw_section].set ? akw[kw_section].val : make_str(wk, "");
	obj dict;

	if (ao[0].set) {
		if (!typecheck(wk, an[0].node, an[0].val, obj_string)) {
			return false;
		}

		make_obj(wk, &dict, obj_dict);
		obj_dict_set(wk, dict, an[0].val, ao[0].val);
	} else {
		if (!typecheck(wk, an[0].node, an[0].val, obj_dict)) {
			return false;
		}

		dict = an[0].val;
	}

	obj prev;
	if (obj_dict_index(wk, current_project(wk)->summary, sec, &prev)) {
		obj ndict;
		obj_dict_merge(wk, prev, dict, &ndict);
		dict = ndict;
	}

	obj_dict_set(wk, current_project(wk)->summary, sec, dict);
	return true;
}

static bool
func_p(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_any }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	obj_printf(wk, "%o\n", an[0].val);
	return true;
}

const struct func_impl_name impl_tbl_default[] =
{
	{ "add_global_arguments", func_add_global_arguments },
	{ "add_global_link_arguments", func_add_global_link_arguments },
	{ "add_languages", func_add_languages },
	{ "add_project_arguments", func_add_project_arguments },
	{ "add_project_link_arguments", func_add_project_link_arguments },
	{ "add_test_setup", todo },
	{ "alias_target", todo },
	{ "assert", func_assert },
	{ "benchmark", todo },
	{ "both_libraries", func_both_libraries },
	{ "build_target", func_build_target },
	{ "configuration_data", func_configuration_data },
	{ "configure_file", func_configure_file },
	{ "custom_target", func_custom_target },
	{ "declare_dependency", func_declare_dependency },
	{ "dependency", func_dependency },
	{ "disabler", func_disabler },
	{ "environment", func_environment },
	{ "error", func_error },
	{ "executable", func_executable },
	{ "files", func_files },
	{ "find_program", func_find_program },
	{ "generator", func_generator },
	{ "get_option", func_get_option },
	{ "get_variable", func_get_variable },
	{ "gettext", todo },
	{ "import", func_import },
	{ "include_directories", func_include_directories },
	{ "install_data", func_install_data },
	{ "install_headers", func_install_headers },
	{ "install_man", func_install_man },
	{ "install_subdir", func_install_subdir },
	{ "is_disabler", func_is_disabler },
	{ "is_variable", func_is_variable },
	{ "jar", todo },
	{ "join_paths", func_join_paths },
	{ "library", func_library },
	{ "message", func_message },
	{ "project", func_project },
	{ "run_command", func_run_command },
	{ "run_target", func_run_target },
	{ "set_variable", func_set_variable },
	{ "shared_library", func_shared_library },
	{ "shared_module", func_shared_module },
	{ "static_library", func_static_library },
	{ "subdir", func_subdir },
	{ "subdir_done", func_subdir_done },
	{ "subproject", func_subproject },
	{ "summary", func_summary },
	{ "test", func_test },
	{ "vcs_tag", func_vcs_tag },
	{ "warning", func_warning },
	{ "p", func_p },
	{ NULL, NULL },
};

const struct func_impl_name impl_tbl_default_external[] = {
	{ "assert", func_assert },
	{ "disabler", func_disabler },
	{ "environment", func_environment },
	{ "error", func_error },
	{ "files", func_files },
	{ "find_program", func_find_program },
	{ "get_variable", func_get_variable },
	{ "import", func_import },
	{ "is_disabler", func_is_disabler },
	{ "is_variable", func_is_variable },
	{ "join_paths", func_join_paths },
	{ "message", func_message },
	{ "p", func_p },
	{ "run_command", func_run_command },
	{ "set_variable", func_set_variable },
	{ "setup", func_setup },
	{ "warning", func_warning },
	{ NULL, NULL },
};

const struct func_impl_name impl_tbl_default_opts[] = {
	{ "option", func_option  },
	{ "p", func_p },
	{ NULL, NULL },
};
