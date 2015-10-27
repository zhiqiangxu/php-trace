/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2015 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_trace.h"

/* If you declare any globals in php_trace.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(trace)
*/

/* True global resources - no need for thread safety here */
static int le_trace;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("trace.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_trace_globals, trace_globals)
    STD_PHP_INI_ENTRY("trace.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_trace_globals, trace_globals)
PHP_INI_END()
*/
/* }}} */

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_trace_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(confirm_trace_compiled)
{
	char *arg = NULL;
	int arg_len, len;
	char *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	len = spprintf(&strg, 0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "trace", arg);
	RETURN_STRINGL(strg, len, 0);
}
/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and 
   unfold functions in source code. See the corresponding marks just before 
   function definition, where the functions purpose is also documented. Please 
   follow this convention for the convenience of others editing your code.
*/


/* {{{ php_trace_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_trace_init_globals(zend_trace_globals *trace_globals)
{
	trace_globals->global_value = 0;
	trace_globals->global_string = NULL;
}
*/
/* }}} */
typedef struct _trace_func {
	char *class;
	char *function;
	int   type;
} trace_func;

#define XFUNC_MEMBER 1
#define XFUNC_NORMAL 2
char *trace_xdebug_sprintf(const char* fmt, ...)
{
	char   *new_str;
	int     size = 1;
	va_list args;

	new_str = (char *) emalloc(size);

	for (;;) {
		int n;

		va_start(args, fmt);
		n = vsnprintf(new_str, size, fmt, args);
		va_end(args);

		if (n > -1 && n < size) {
			break;
		}
		if (n < 0) {
			size *= 2;
		} else {
			size = n + 1;
		}
		new_str = (char *) erealloc(new_str, size);
	}

	return new_str;
}
static void trace_xdebug_build_fname_from_oparray(trace_func *tmp, zend_op_array *opa TSRMLS_DC)
{
	int closure = 0;

	memset(tmp, 0, sizeof(trace_func));

	if (opa->function_name) {
		if (strcmp(opa->function_name, "{closure}") == 0) {
			tmp->function = trace_xdebug_sprintf(
				"{closure:%s:%d-%d}",
				opa->filename,
				opa->line_start,
				opa->line_end
			);
			closure = 1;
		} else {
			tmp->function = estrdup(opa->function_name);
		}
	} else {
		tmp->function = estrdup("{main}");
	}

	if (opa->scope && !closure) {
		tmp->type = XFUNC_MEMBER;
		tmp->class = estrdup(opa->scope->name);
	} else {
		tmp->type = XFUNC_NORMAL;
	}
	//php_printf("trace_func: %s %s\n", tmp->class, tmp->function);
}
static void debug_backtrace()
{
	zval return_value;
	INIT_ZVAL(return_value);
	long options = DEBUG_BACKTRACE_PROVIDE_OBJECT;
	long limit = 0;

	zend_fetch_debug_backtrace(&return_value, 1, options, limit TSRMLS_CC);
	zend_print_zval_r(&return_value, 0 TSRMLS_CC);
	zval_dtor(&return_value);
}

static void trace_print_current_lineno()
{
	zend_execute_data *ptr = EG(current_execute_data);
	zend_op_array *op_array = ptr->op_array;
	const char *file = op_array->filename;
	const zend_op *cur_opcode = *EG(opline_ptr);
	int lineno = cur_opcode->lineno;
	char *log = trace_xdebug_sprintf("file:\t%s lineno:\t%d\n", file, lineno);
	php_log_err(log);
	efree(log);
}

trace_func func_info;
void trace_func_dtor(trace_func *f)
{
	if (f->type == XFUNC_MEMBER) {
		efree(f->class);
		efree(f->function);
		f->class = f->function = NULL;
	} else if (f->type == XFUNC_NORMAL) {
		efree(f->function);
		f->function = NULL;
	}
}

static void trace_get_current_function(trace_func *func_info)
{
	zend_execute_data *ptr = EG(current_execute_data);
	zend_op_array *op_array = ptr->op_array;
	trace_xdebug_build_fname_from_oparray(func_info, op_array TSRMLS_CC);
}

static int trace_is_func_equal(trace_func *f1, trace_func *f2)
{
	if (f1->type == XFUNC_MEMBER) {
		return (f1->type == f2->type && strcmp(f1->class, f2->class) == 0 && strcmp(f1->function, f2->function) == 0);
	} else {
		return (f1->type == f2->type && strcmp(f1->function, f2->function) == 0);
	}
}
static int trace_stmt_handler(ZEND_OPCODE_HANDLER_ARGS)
{
	//php_printf("stmt called\n");

	trace_func tmp_func_info;
	trace_get_current_function(&tmp_func_info);

	//php_printf("%s %s %d\n", tmp_func_info.class, tmp_func_info.function, tmp_func_info.type);
	//php_printf("%s %s %d\n", func_info.class, func_info.function, func_info.type);
	if (trace_is_func_equal(&tmp_func_info, &func_info)) {
		trace_print_current_lineno();
	}
	trace_func_dtor(&tmp_func_info);
	return ZEND_USER_OPCODE_DISPATCH;
}

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(trace)
{
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/

	//trace_print_current_lineno();
	trace_get_current_function(&func_info);
	//debug_backtrace();
	
	zend_execute_data* ptr = EG(current_execute_data);
	//php_printf("opcode: %d\n", ptr->opline->opcode);
	//php_printf("function_name:%s\n", ptr->function_state.function->common.function_name);
	zend_set_user_opcode_handler(ZEND_EXT_STMT, trace_stmt_handler);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(trace)
{
	//php_printf("trace mshutdown\n");
	trace_func_dtor(&func_info);
	zend_set_user_opcode_handler(ZEND_EXT_STMT, NULL);
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(trace)
{
	//php_printf("trace rinit\n");
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(trace)
{
	//php_printf("trace rshutdown\n");
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(trace)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "trace support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ trace_functions[]
 *
 * Every user visible function must have an entry in trace_functions[].
 */
const zend_function_entry trace_functions[] = {
	PHP_FE(confirm_trace_compiled,	NULL)		/* For testing, remove later. */
	PHP_FE_END	/* Must be the last line in trace_functions[] */
};
/* }}} */

/* {{{ trace_module_entry
 */
zend_module_entry trace_module_entry = {
	STANDARD_MODULE_HEADER,
	"trace",
	trace_functions,
	PHP_MINIT(trace),
	PHP_MSHUTDOWN(trace),
	PHP_RINIT(trace),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(trace),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(trace),
	PHP_TRACE_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_TRACE
ZEND_GET_MODULE(trace)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
