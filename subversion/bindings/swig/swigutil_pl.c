/*
 * swigutil_py.c: utility functions for the SWIG Perl bindings
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include <stdarg.h>

#include <apr.h>
#include <apr_general.h>

#include "svn_pools.h"
#include "svn_opt.h"

#include "swigutil_pl.h"

const apr_hash_t *svn_swig_pl_objs_to_hash(SV *source, swig_type_info *tinfo,
					   apr_pool_t *pool)
{
    apr_hash_t *hash = apr_hash_make (pool);
    HV *h = (HV *)SvRV(source);
    char *key;
    I32 cnt = hv_iterinit(h), retlen;

    while (cnt--) {
	SV* item = hv_iternextsv(h, &key, &retlen);
	void **val = apr_palloc(pool, sizeof(void *));
	if (SWIG_ConvertPtr(item, val, tinfo, 0) < 0) {
	    croak("aahhh");
	}
	apr_hash_set (hash, key, APR_HASH_KEY_STRING, *val);
    }

    return hash;
}

const apr_hash_t *svn_swig_pl_objs_to_hash_by_name(SV *source,
						   const char *typename,
						   apr_pool_t *pool)
{
    swig_type_info *tinfo = SWIG_TypeQuery(typename);
    return svn_swig_pl_objs_to_hash (source, tinfo, pool);
}



typedef SV *(*element_converter_t)(void *value, void *ctx);

SV *convert_hash (apr_hash_t *hash, element_converter_t converter_func,
		  void *ctx)
{
    apr_hash_index_t *hi;
    HV *hv;

    hv = newHV();
    for (hi = apr_hash_first(NULL, hash); hi; hi = apr_hash_next(hi)) {
	const char *key;
	void *val;
	int klen;
	SV *obj;

	apr_hash_this(hi, (void *)&key, NULL, &val);
	klen = strlen(key);

	obj = converter_func (val, ctx);
	hv_store(hv, (const char *)key, klen, obj, 0);
	SvREFCNT_inc(obj);
    }
    
    return newRV_inc((SV*)hv);
}


SV *convert_string (const char *value, void *dummy)
{
    SV *obj = sv_2mortal(newSVpv(value, 0));
    return obj;
}

SV *convert_svn_string_t (svn_string_t *value, void *dummy)
{
    SV *obj = sv_2mortal(newSVpv(value->data, value->len));
    return obj;
}

SV *convert_to_swig_type (void *ptr, swig_type_info *tinfo)
{
    SV *obj = sv_newmortal();
    SWIG_MakePtr(obj, ptr, tinfo, 0);
    return obj;
}

SV *svn_swig_pl_prophash_to_hash (apr_hash_t *hash)
{
    return convert_hash (hash, (element_converter_t)convert_svn_string_t,
			 NULL);
}

SV *svn_swig_pl_convert_hash (apr_hash_t *hash, swig_type_info *tinfo)
{
    return convert_hash (hash, (element_converter_t)convert_to_swig_type,
			 tinfo);
}
const apr_array_header_t *svn_swig_pl_strings_to_array(SV *source,
                                                       apr_pool_t *pool)
{
    int targlen;
    apr_array_header_t *temp;
    AV* array;

    if (!(source && SvROK(source) && SvTYPE(SvRV(source)) == SVt_PVAV)) {
	/* raise exception here */
	return NULL;
    }
    array = (AV *)SvRV (source);
    targlen = av_len (array) + 1;
    temp = apr_array_make (pool, targlen, sizeof(const char *));
    temp->nelts = targlen;

    while (targlen--) {
	/* more error handling here */
	SV **item = av_fetch (array, targlen, 0);
        APR_ARRAY_IDX(temp, targlen, const char *) = SvPV_nolen (*item);
    }

    return temp;
}

const apr_array_header_t *svn_swig_pl_objs_to_array(SV *source,
						    swig_type_info *tinfo,
						    apr_pool_t *pool)
{
    int targlen;
    apr_array_header_t *temp;
    AV* array;

    if (!(source && SvROK(source) && SvTYPE(SvRV(source)) == SVt_PVAV)) {
	/* raise exception here */
	return NULL;
    }
    array = (AV *)SvRV (source);
    targlen = av_len (array) + 1;
    temp = apr_array_make (pool, targlen, sizeof(const char *));
    temp->nelts = targlen;

    while (targlen--) {
	/* more error handling here */
	SV **item = av_fetch (array, targlen, 0);
	void **obj = apr_palloc(pool, sizeof(void *));
	if (SWIG_ConvertPtr(*item, (void **) obj, tinfo,0) < 0) {
	    croak("aahhh");
	}
        APR_ARRAY_IDX(temp, targlen, void *) = *obj;
    }

    return temp;
}

SV *convert_array(const apr_array_header_t *array,
		  element_converter_t converter_func, void *ctx)
{
    AV *list = newAV();
    int i;

    for (i = 0; i < array->nelts; ++i) {
	void *element = APR_ARRAY_IDX(array, i, void *);
	SV *item = converter_func (element, ctx);
	av_push (list, item);
	SvREFCNT_inc (item);
    }
    return newRV_inc((SV*)list);
}

SV *svn_swig_pl_array_to_list(const apr_array_header_t *array)
{
    return convert_array (array, (element_converter_t)convert_string, NULL);
}

SV *convert_int(int value, void *dummy)
{
    return newSViv (value);
}

SV *svn_swig_pl_ints_to_list(const apr_array_header_t *array)
{
    return convert_array (array, (element_converter_t)convert_int, NULL);
}

typedef enum perl_func_invoker {
    CALL_METHOD,
    CALL_SV,
} perl_func_invoker_t;

/* put the va_arg in stack and invoke caller_func with func.
   fmt:
   * O: perl object
   * i: integer
   * s: string
   * S: swigtype

   put returned value in result if result is not NULL
*/

static svn_error_t *perl_callback_thunk (perl_func_invoker_t caller_func,
					 void *func,
					 SV **result,
					 const char *fmt, ...)
{
    const char *fp = fmt;
    va_list ap;
    int count;

    dSP ;
    ENTER ;
    SAVETMPS ;

    PUSHMARK(SP) ;

    va_start(ap, fmt);
    while (*fp) {
	char *c;
	void *o;
	SV *obj;
	swig_type_info *t;

	switch (*fp++) {
	case 'O':
	    XPUSHs (va_arg (ap, SV *));
	    break;
	case 'S': /* swig object */
	    o = va_arg (ap, void *);
	    t = va_arg (ap, swig_type_info *);
  
	    obj = sv_newmortal ();
	    SWIG_MakePtr (obj, o, t, 0);
	    XPUSHs(obj);
	    break;

	case 's': /* string */
	    c = va_arg (ap, char *);
	    XPUSHs(c ? sv_2mortal(newSVpv(c, 0)) : &PL_sv_undef);
	    break;

	case 'i': /* integer */
	    XPUSHs(sv_2mortal(newSViv(va_arg(ap, int))));
	    break;

	}
    }

    va_end (ap);

    PUTBACK;
    switch (caller_func) {
    case CALL_SV:
	count = call_sv (func, G_SCALAR);
	break;
    case CALL_METHOD:
	count = call_method (func, G_SCALAR);
	break;
    }
    SPAGAIN ;

    if (count != 1)
	croak("Big trouble\n") ;

    if (result) {
	*result = POPs;
	SvREFCNT_inc(*result);
    }

    FREETMPS ;
    LEAVE ;

    return SVN_NO_ERROR;
}


/*** Editor Wrapping ***/

/* this could be more perlish */
typedef struct {
    SV *editor;     /* the editor handling the callbacks */
    SV *baton;      /* the dir/file baton (or NULL for edit baton) */
} item_baton;

static item_baton * make_baton(apr_pool_t *pool,
                               SV *editor, SV *baton)
{
    item_baton *newb = apr_palloc(pool, sizeof(*newb));

    SvREFCNT_inc(editor);

    newb->editor = editor;
    newb->baton = baton;

    return newb;
}

static svn_error_t * close_baton(void *baton, const char *method)
{
    item_baton *ib = baton;
    dSP ;

    ENTER ;
    SAVETMPS ;

    PUSHMARK(SP) ;
    XPUSHs(ib->editor);

    if (ib->baton)
	XPUSHs(ib->baton);

    PUTBACK;

    call_method(method, G_DISCARD);

    /* check result? */

    SvREFCNT_dec(ib->editor);
    if (ib->baton)
	SvREFCNT_dec(ib->baton);

#ifdef SVN_DEBUG
    ib->editor = ib->baton = NULL;
#endif

    FREETMPS ;
    LEAVE ;

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_set_target_revision(void *edit_baton,
                                               svn_revnum_t target_revision,
                                               apr_pool_t *pool)
{
    item_baton *ib = edit_baton;

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "set_target_revision", NULL,
				  "Oi", ib->editor, target_revision));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_root(void *edit_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *dir_pool,
                                     void **root_baton)
{
    item_baton *ib = edit_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "open_root", &result,
				  "OiS", ib->editor, base_revision,
				  dir_pool, poolinfo));

    *root_baton = make_baton(dir_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_delete_entry(const char *path,
                                        svn_revnum_t revision,
                                        void *parent_baton,
                                        apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "delete_entry", NULL,
				  "OsiOS", ib->editor, path, revision,
				  ib->baton, pool, poolinfo));
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_add_directory(const char *path,
                                         void *parent_baton,
                                         const char *copyfrom_path,
                                         svn_revnum_t copyfrom_revision,
                                         apr_pool_t *dir_pool,
                                         void **child_baton)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "add_directory", &result,
				  "OsOsiS", ib->editor, path, ib->baton,
				  copyfrom_path, copyfrom_revision, 
				  dir_pool, poolinfo));
    *child_baton = make_baton(dir_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_directory(const char *path,
                                          void *parent_baton,
                                          svn_revnum_t base_revision,
                                          apr_pool_t *dir_pool,
                                          void **child_baton)
{
    item_baton *ib = parent_baton;
    SV *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "open_directory", &result,
				  "OsOiS", ib->editor, path, ib->baton,
				  base_revision, dir_pool, poolinfo));

    *child_baton = make_baton(dir_pool, ib->editor, result);

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_dir_prop(void *dir_baton,
                                           const char *name,
                                           const svn_string_t *value,
                                           apr_pool_t *pool)
{
    item_baton *ib = dir_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "change_dir_prop", NULL,
				  "OOssS", ib->editor, ib->baton, name,
				  value ? value->data : NULL,
				  pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_directory(void *dir_baton,
                                           apr_pool_t *pool)
{
    return close_baton(dir_baton, "close_directory");
}

static svn_error_t * thunk_absent_directory(const char *path,
					    void *parent_baton,
					    apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "absent_directory", NULL,
				  "OsOS", ib->editor, path, ib->baton,
				  pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_add_file(const char *path,
                                    void *parent_baton,
                                    const char *copyfrom_path,
                                    svn_revnum_t copyfrom_revision,
                                    apr_pool_t *file_pool,
                                    void **file_baton)
{
    item_baton *ib = parent_baton;
    SV *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "add_file", &result,
				  "OsOsiS", ib->editor, path, ib->baton,
				  copyfrom_path, copyfrom_revision,
				  file_pool, poolinfo));

    *file_baton = make_baton(file_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_file(const char *path,
                                     void *parent_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *file_pool,
                                     void **file_baton)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "open_file", &result,
				  "OsOiS", ib->editor, path, ib->baton,
				  base_revision, file_pool, poolinfo));

    *file_baton = make_baton(file_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_window_handler(svn_txdelta_window_t *window,
                                          void *baton)
{
    SV *handler = baton;

    if (window == NULL) {
	SVN_ERR (perl_callback_thunk (CALL_SV,
				      handler, NULL, "O",
				      &PL_sv_undef));
    }
    else {
	swig_type_info *tinfo = SWIG_TypeQuery("svn_txdelta_window_t *");
	SVN_ERR (perl_callback_thunk (CALL_SV,
				      handler, NULL, "S", window, tinfo));
    }

    return SVN_NO_ERROR;
}

static svn_error_t *
thunk_apply_textdelta(void *file_baton, 
                      const char *base_checksum,
                      apr_pool_t *pool,
                      svn_txdelta_window_handler_t *handler,
                      void **h_baton)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "apply_textdelta", &result,
				  "OOsS", ib->editor, ib->baton, base_checksum,
				  pool, poolinfo));
    if (SvOK(result)) {
	if (SvROK(result) && SvTYPE(SvRV(result)) == SVt_PVAV) {
	    swig_type_info *handler_info = SWIG_TypeQuery("svn_txdelta_window_handler_t"), *void_info = SWIG_TypeQuery("void *");
	    AV *array = (AV *)SvRV(result);

	    if (SWIG_ConvertPtr(*av_fetch (array, 0, 0),
				(void **)handler, handler_info,0) < 0) {
		croak("FOO!");
	    }
	    if (SWIG_ConvertPtr(*av_fetch (array, 1, 0),
				h_baton, void_info,0) < 0) {
		croak("FOO!");
	    }
	}
	else {
	    *handler = thunk_window_handler;
	    *h_baton = result;
	}
    }
    else {
	*handler = svn_delta_noop_window_handler;
	*h_baton = NULL;
    }

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_file_prop(void *file_baton,
                                            const char *name,
                                            const svn_string_t *value,
                                            apr_pool_t *pool)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "change_file_prop", NULL,
				  "OOssS", ib->editor, ib->baton, name,
				  value ? value->data : NULL,
				  pool, poolinfo));
  
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_file(void *file_baton,
                                      const char *text_checksum,
                                      apr_pool_t *pool)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "close_file", NULL, "OOsS",
				  ib->editor, ib->baton, text_checksum,
				  pool, poolinfo));

    SvREFCNT_dec(ib->editor);
    SvREFCNT_dec(ib->baton);

#ifdef SVN_DEBUG
    ib->editor = ib->baton = NULL;
#endif

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_absent_file(const char *path,
				       void *parent_baton,
				       apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (perl_callback_thunk (CALL_METHOD,
				  "absent_file", NULL,
				  "OsOS", ib->editor, path, ib->baton,
				  pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_edit(void *edit_baton,
                                      apr_pool_t *pool)
{
    return close_baton(edit_baton, "close_edit");
}

static svn_error_t * thunk_abort_edit(void *edit_baton,
                                      apr_pool_t *pool)
{
    return close_baton(edit_baton, "abort_edit");
}

void svn_delta_make_editor(const svn_delta_editor_t **editor,
			   void **edit_baton,
			   SV *perl_editor,
			   apr_pool_t *pool)
{
    svn_delta_editor_t *thunk_editor = svn_delta_default_editor (pool);
  
    thunk_editor->set_target_revision = thunk_set_target_revision;
    thunk_editor->open_root = thunk_open_root;
    thunk_editor->delete_entry = thunk_delete_entry;
    thunk_editor->add_directory = thunk_add_directory;
    thunk_editor->open_directory = thunk_open_directory;
    thunk_editor->change_dir_prop = thunk_change_dir_prop;
    thunk_editor->close_directory = thunk_close_directory;
    thunk_editor->absent_directory = thunk_absent_directory;
    thunk_editor->add_file = thunk_add_file;
    thunk_editor->open_file = thunk_open_file;
    thunk_editor->apply_textdelta = thunk_apply_textdelta;
    thunk_editor->change_file_prop = thunk_change_file_prop;
    thunk_editor->close_file = thunk_close_file;
    thunk_editor->absent_file = thunk_absent_file;
    thunk_editor->close_edit = thunk_close_edit;
    thunk_editor->abort_edit = thunk_abort_edit;

    *editor = thunk_editor;
    *edit_baton = make_baton(pool, perl_editor, NULL);
}

svn_error_t *svn_swig_pl_thunk_log_receiver(void *baton,
					    apr_hash_t *changed_paths,
					    svn_revnum_t rev,
					    const char *author,
					    const char *date,
					    const char *msg,
					    apr_pool_t *pool)
{
    SV *receiver = baton, *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    swig_type_info *tinfo = SWIG_TypeQuery("svn_log_changed_path_t *");

    if (!SvOK(receiver))
	return SVN_NO_ERROR;

    perl_callback_thunk (CALL_SV,
			 receiver, &result,
			 "OisssS", (changed_paths) ?
			 svn_swig_pl_convert_hash(changed_paths, tinfo)
			 : &PL_sv_undef,
			 rev, author, date, msg, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_history_func(void *baton,
                                            const char *path,
                                            svn_revnum_t revision,
                                            apr_pool_t *pool)
{
    SV *func = baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    if (!SvOK(func))
	return SVN_NO_ERROR;

    perl_callback_thunk (CALL_SV,
			 func, NULL,
			 "siS", path, revision, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_authz_read_func (svn_boolean_t *allowed,
						svn_fs_root_t *root,
						const char *path,
						void *baton,
						apr_pool_t *pool)
{
    SV *func = baton, *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    swig_type_info *rootinfo = SWIG_TypeQuery("svn_fs_root_t *");

    if (!SvOK(func))
	return SVN_NO_ERROR;

    perl_callback_thunk (CALL_SV,
			 func, &result,
			 "SsS", root, rootinfo, path, pool, poolinfo);

    *allowed = SvIV (result);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_commit_callback(svn_revnum_t new_revision,
					       const char *date,
					       const char *author,
					       void *baton)
{
    if (!SvOK((SV *)baton))
	return SVN_NO_ERROR;

    perl_callback_thunk (CALL_SV, baton, NULL,
			 "iss", new_revision, date, author);

    return SVN_NO_ERROR;
}

/* Wrap RA */

static svn_error_t * thunk_open_tmp_file(apr_file_t **fp,
					 void *callback_baton,
					 apr_pool_t *pool)
{
    SV *result;
    swig_type_info *tinfo = SWIG_TypeQuery("apr_file_t *");

    perl_callback_thunk (CALL_METHOD, "open_tmp_file",
			 &result, "O", callback_baton);

    if (SWIG_ConvertPtr(result, (void *)fp, tinfo,0) < 0) {
	croak("ouch");
    }

    return SVN_NO_ERROR;
}

svn_error_t *thunk_get_wc_prop (void *baton,
				const char *relpath,
				const char *name,
				const svn_string_t **value,
				apr_pool_t *pool)
{
    SV *result;
    swig_type_info *tinfo = SWIG_TypeQuery("apr_pool_t *");

    perl_callback_thunk (CALL_METHOD, "get_wc_prop",
			 &result, "OssS", baton, relpath, name, pool, tinfo);

    /* this is svn_string_t * typemap in */
    if (!SvOK (result) || result == &PL_sv_undef) {
	*value = NULL;
    }
    else if (SvPOK(result)) {
	*value = svn_string_create (SvPV_nolen (result), pool);
    }
    else {
	croak("not a string");
    }

    return SVN_NO_ERROR;
}


svn_error_t *svn_ra_make_callbacks(svn_ra_callbacks_t **cb,
				   void **c_baton,
				   SV *perl_callbacks,
				   apr_pool_t *pool)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_auth_baton_t *");
    SV *auth_baton;

    *cb = apr_pcalloc (pool, sizeof(**cb));

    (*cb)->open_tmp_file = thunk_open_tmp_file;
    (*cb)->get_wc_prop = thunk_get_wc_prop;
    (*cb)->set_wc_prop = NULL;
    (*cb)->push_wc_prop = NULL;
    (*cb)->invalidate_wc_props = NULL;
    auth_baton = *hv_fetch((HV *)SvRV(perl_callbacks), "auth", 4, 0);

    if (SWIG_ConvertPtr(auth_baton, (void **)&(*cb)->auth_baton, tinfo,0) < 0) {
	croak("ouch");
    }
    *c_baton = perl_callbacks;
    SvREFCNT_inc(perl_callbacks);

    return SVN_NO_ERROR;
}

/* default pool support */
apr_pool_t *current_pool;

apr_pool_t *svn_swig_pl_make_pool (SV *obj)
{
    apr_pool_t *pool;

    if (obj && sv_isobject (obj)) {
	swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
	if (sv_derived_from (obj, "SVN::Pool")) {
	    obj = SvRV(obj);
	}
	if (sv_derived_from(obj, "_p_apr_pool_t")) {
	    SWIG_ConvertPtr(obj, (void **)&pool, poolinfo, 0);
	    return pool;
	}
    }

    if (!current_pool)
	perl_callback_thunk (CALL_METHOD, "new_default",
			     &obj, "s", "SVN::Pool");
    pool = current_pool;

    return pool;
}

/* stream interpolability with io::handle */

typedef struct  {
    SV *obj;
    IO *io;
    apr_pool_t *pool;
} io_baton_t;

static svn_error_t *io_handle_read (void *baton,
				    char *buffer,
				    apr_size_t *len)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if (mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar)) {
	SV *ret;
	SV *buf = sv_newmortal();
	perl_callback_thunk (CALL_METHOD, "READ", &ret, "OOi",
			     SvTIED_obj((SV*)io->io, mg),
			     buf, *len);
	*len = SvIV (ret);
	memmove (buffer, SvPV_nolen(buf), *len);
    }
    else
	*len = PerlIO_read (IoIFP (io->io), buffer, *len);
    return SVN_NO_ERROR;
}

static svn_error_t *io_handle_write (void *baton,
				     const char *data,
				     apr_size_t *len)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if (mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar)) {
	SV *ret;
	perl_callback_thunk (CALL_METHOD, "WRITE", &ret, "Osi",
			     SvTIED_obj((SV*)io->io, mg), data, *len);
	*len = SvIV (ret);
    }
    else
	*len = PerlIO_write (IoIFP (io->io), data, *len);
    return SVN_NO_ERROR;
}

static svn_error_t *io_handle_close (void *baton)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if (mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar)) {
	perl_callback_thunk (CALL_METHOD, "CLOSE", NULL, "O",
			     SvTIED_obj((SV*)io->io, mg));
    }
    else {
	PerlIO_close (IoIFP (io->io));
	SvREFCNT_dec (io->obj);
    }
    apr_pool_destroy (io->pool);
    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_make_stream (svn_stream_t **stream, SV *obj)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_stream_t *");
    IO *io;

    if (obj && SvROK(obj) && SvTYPE(SvRV(obj)) == SVt_PVGV &&
	(io = GvIO(SvRV(obj)))) {
	apr_pool_t *pool = svn_pool_create (NULL);
	io_baton_t *iob = apr_palloc (pool, sizeof(io_baton_t));
	SvREFCNT_inc (obj);
	iob->obj = obj;
	iob->io = io;
	iob->pool = pool;
	*stream = svn_stream_create (iob, pool);
	svn_stream_set_read (*stream, io_handle_read);
	svn_stream_set_write (*stream, io_handle_write);
	svn_stream_set_close (*stream, io_handle_close);
	return SVN_NO_ERROR;
    }

    if (obj && sv_isobject(obj)) {
	if (sv_derived_from (obj, "SVN::Stream"))
	    perl_callback_thunk (CALL_METHOD, "svn_stream", &obj, "O", obj);
	else if (!sv_derived_from(obj, "_p_svn_stream_t"))
	    croak ("unknown type for svn_stream_t");

	SWIG_ConvertPtr(obj, (void **)stream, tinfo, 0);
    }
    else {
	croak ("unknown type for svn_stream_t");
    }
    return SVN_NO_ERROR;
}

SV *svn_swig_pl_from_stream (svn_stream_t *stream)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_stream_t *");
    SV *ret;

    perl_callback_thunk (CALL_METHOD, "new", &ret, "sS",
			 "SVN::Stream", stream, tinfo);

    return sv_2mortal (ret);
}

apr_file_t *svn_swig_pl_make_file (SV *file, apr_pool_t *pool)
{
    apr_file_t *apr_file = NULL;

    if (!SvOK(file) || file == &PL_sv_undef)
	return NULL;

    if (SvPOKp(file)) {
      apr_file_open(&apr_file, SvPV_nolen(file),
                    APR_CREATE | APR_READ | APR_WRITE,
                    APR_OS_DEFAULT,
                    pool);
    }
    else {
	croak ("apr_file_t conversion from non-string not supported yet");
    }

    return apr_file;
}
