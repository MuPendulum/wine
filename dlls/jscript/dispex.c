/*
 * Copyright 2008 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <limits.h>

#include "jscript.h"
#include "engine.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(jscript);

static const GUID GUID_JScriptTypeInfo = {0xc59c6b12,0xf6c1,0x11cf,{0x88,0x35,0x00,0xa0,0xc9,0x11,0xe8,0xb2}};

#define FDEX_VERSION_MASK 0xf0000000
#define GOLDEN_RATIO 0x9E3779B9U

typedef enum {
    PROP_JSVAL,
    PROP_BUILTIN,
    PROP_PROTREF,
    PROP_ACCESSOR,
    PROP_PROXY,
    PROP_DELETED,
    PROP_IDX
} prop_type_t;

struct _dispex_prop_t {
    WCHAR *name;
    unsigned hash;
    prop_type_t type;
    DWORD flags;

    union {
        jsval_t val;
        const builtin_prop_t *p;
        DWORD ref;
        unsigned idx;
        struct {
            jsdisp_t *getter;
            jsdisp_t *setter;
        } accessor;
        DISPID proxy_id;
    } u;

    int bucket_head;
    int bucket_next;
};

static HRESULT fix_overridden_prop(jsdisp_t *This, dispex_prop_t *prop);

static void fix_protref_prop(jsdisp_t *jsdisp, dispex_prop_t *prop)
{
    DWORD ref;

    if(prop->type != PROP_PROTREF)
        return;
    ref = prop->u.ref;

    while((jsdisp = jsdisp->prototype)) {
        if(ref >= jsdisp->prop_cnt || jsdisp->props[ref].type == PROP_DELETED)
            break;
        if(jsdisp->props[ref].type != PROP_PROTREF)
            return;
        ref = jsdisp->props[ref].u.ref;
    }
    prop->type = PROP_DELETED;
}

static inline DISPID prop_to_id(jsdisp_t *This, dispex_prop_t *prop)
{
    /* don't overlap with DISPID_VALUE */
    return prop - This->props + 1;
}

static inline dispex_prop_t *get_prop(jsdisp_t *This, DISPID id)
{
    dispex_prop_t *prop;
    DWORD idx = id - 1;

    if(idx >= This->prop_cnt)
        return NULL;
    prop = &This->props[idx];

    fix_overridden_prop(This, prop);
    fix_protref_prop(This, prop);
    return prop->type == PROP_DELETED ? NULL : prop;
}

static inline BOOL is_function_prop(dispex_prop_t *prop)
{
    BOOL ret = FALSE;

    if (is_object_instance(prop->u.val))
    {
        jsdisp_t *jsdisp = to_jsdisp(get_object(prop->u.val));

        if (jsdisp) ret = is_class(jsdisp, JSCLASS_FUNCTION);
    }
    return ret;
}

static inline BOOL override_idx(jsdisp_t *This, const WCHAR *name, unsigned *ret_idx)
{
    /* Typed Arrays override every positive index */
    if(This->builtin_info->class >= FIRST_TYPEDARRAY_JSCLASS && This->builtin_info->class <= LAST_TYPEDARRAY_JSCLASS) {
        const WCHAR *ptr;
        unsigned idx = 0;

        for(ptr = name; is_digit(*ptr) && idx <= (UINT_MAX-9 / 10); ptr++)
            idx = idx*10 + (*ptr-'0');
        if(!*ptr) {
            *ret_idx = idx;
            return TRUE;
        }else {
            while(is_digit(*ptr)) ptr++;
            if(!*ptr) {
                *ret_idx = UINT_MAX;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static DWORD get_flags(jsdisp_t *This, dispex_prop_t *prop)
{
    if(prop->type == PROP_PROTREF) {
        dispex_prop_t *parent = NULL;

        if(prop->u.ref < This->prototype->prop_cnt)
            parent = &This->prototype->props[prop->u.ref];

        if(!parent || parent->type == PROP_DELETED) {
            prop->type = PROP_DELETED;
            return 0;
        }

        return get_flags(This->prototype, parent);
    }

    return prop->flags;
}

static const builtin_prop_t *find_builtin_prop(jsdisp_t *This, const WCHAR *name, BOOL case_insens)
{
    int min = 0, max = This->builtin_info->props_cnt-1, i, r;
    unsigned version;

    if(case_insens) {
        for(i = min; i <= max; i++)
            if(!wcsicmp(name, This->builtin_info->props[i].name))
                goto found;
        return NULL;
    }

    while(min <= max) {
        i = (min+max)/2;

        r = wcscmp(name, This->builtin_info->props[i].name);
        if(!r)
            goto found;

        if(r < 0)
            max = i-1;
        else
            min = i+1;
    }

    return NULL;

found:
    /* Skip prop if it's available only in higher compatibility mode. */
    version = (This->builtin_info->props[i].flags & PROPF_VERSION_MASK) >> PROPF_VERSION_SHIFT;
    if(version && version > This->ctx->version)
        return NULL;

    /* Skip prop if it's available only in HTML mode and we're not running in HTML mode. */
    if((This->builtin_info->props[i].flags & PROPF_HTML) && !This->ctx->html_mode)
        return NULL;

    return This->builtin_info->props + i;
}

static inline unsigned string_hash(const WCHAR *name)
{
    unsigned h = 0;
    for(; *name; name++)
        h = (h>>(sizeof(unsigned)*8-4)) ^ (h<<4) ^ towlower(*name);
    return h;
}

static inline unsigned get_props_idx(jsdisp_t *This, unsigned hash)
{
    return (hash*GOLDEN_RATIO) & (This->buf_size-1);
}

static inline HRESULT resize_props(jsdisp_t *This)
{
    dispex_prop_t *props;
    int i, bucket;

    if(This->buf_size != This->prop_cnt)
        return S_FALSE;

    props = realloc(This->props, sizeof(dispex_prop_t) * This->buf_size * 2);
    if(!props)
        return E_OUTOFMEMORY;
    This->buf_size *= 2;
    This->props = props;

    for(i=0; i<This->buf_size; i++) {
        This->props[i].bucket_head = ~0;
        This->props[i].bucket_next = ~0;
    }

    for(i=0; i<This->prop_cnt; i++) {
        props = This->props+i;

        bucket = get_props_idx(This, props->hash);
        props->bucket_next = This->props[bucket].bucket_head;
        This->props[bucket].bucket_head = i;
    }

    return S_OK;
}

static inline dispex_prop_t* alloc_prop(jsdisp_t *This, const WCHAR *name, prop_type_t type, DWORD flags)
{
    dispex_prop_t *prop;
    unsigned bucket;

    if(FAILED(resize_props(This)))
        return NULL;

    prop = &This->props[This->prop_cnt];
    prop->name = wcsdup(name);
    if(!prop->name)
        return NULL;
    prop->type = type;
    prop->flags = flags;
    prop->hash = string_hash(name);

    bucket = get_props_idx(This, prop->hash);
    prop->bucket_next = This->props[bucket].bucket_head;
    This->props[bucket].bucket_head = This->prop_cnt++;
    return prop;
}

static HRESULT alloc_proxy_prop(jsdisp_t *This, struct proxy_prop_info *info, dispex_prop_t **ret)
{
    dispex_prop_t *prop;
    jsdisp_t *funcs[2];
    prop_type_t type;
    HRESULT hres;

    if(!info->func[0].invoke)
        type = PROP_PROXY;
    else {
        hres = create_proxy_functions(This, info, funcs);
        if(FAILED(hres))
            return hres;
        type = (info->flags & PROPF_METHOD) ? PROP_JSVAL : PROP_ACCESSOR;
    }

    if((prop = *ret)) {
        prop->type = type;
        prop->flags = info->flags & PROPF_ALL;
    }else {
        prop = alloc_prop(This, info->name, type, info->flags & PROPF_ALL);
        if(!prop) {
            if(type != PROP_PROXY) {
                jsdisp_release(funcs[0]);
                if(funcs[1])
                    jsdisp_release(funcs[1]);
            }
            return E_OUTOFMEMORY;
        }
        *ret = prop;
    }

    if(type == PROP_PROXY)
        prop->u.proxy_id = info->dispid;
    else if(type == PROP_JSVAL)
        prop->u.val = jsval_obj(funcs[0]);
    else {
        prop->u.accessor.getter = funcs[0];
        prop->u.accessor.setter = funcs[1];
    }

    return S_OK;
}

static dispex_prop_t *alloc_protref(jsdisp_t *This, const WCHAR *name, DWORD ref)
{
    dispex_prop_t *ret;

    ret = alloc_prop(This, name, PROP_PROTREF, 0);
    if(!ret)
        return NULL;

    ret->u.ref = ref;
    return ret;
}

static dispex_prop_t *find_prop_name_raw(jsdisp_t *This, unsigned hash, const WCHAR *name, BOOL case_insens)
{
    unsigned bucket, pos, prev = ~0;

    bucket = get_props_idx(This, hash);
    pos = This->props[bucket].bucket_head;
    while(pos != ~0) {
        if(case_insens ? !wcsicmp(name, This->props[pos].name) : !wcscmp(name, This->props[pos].name)) {
            if(prev != ~0) {
                This->props[prev].bucket_next = This->props[pos].bucket_next;
                This->props[pos].bucket_next = This->props[bucket].bucket_head;
                This->props[bucket].bucket_head = pos;
            }

            return &This->props[pos];
        }

        prev = pos;
        pos = This->props[pos].bucket_next;
    }
    return NULL;
}

static HRESULT find_prop_name(jsdisp_t *This, unsigned hash, const WCHAR *name, BOOL case_insens, dispex_prop_t **ret)
{
    dispex_prop_t *prop = find_prop_name_raw(This, hash, name, case_insens);
    const builtin_prop_t *builtin;
    HRESULT hres;

    if(prop) {
        hres = fix_overridden_prop(This, prop);
        *ret = prop;
        return hres;
    }

    if(This->proxy) {
        struct proxy_prop_info info;
        hres = This->proxy->lpVtbl->PropGetInfo(This->proxy, name, case_insens, &info);
        if(hres == S_OK) {
            *ret = NULL;
            return alloc_proxy_prop(This, &info, ret);
        }
        if(hres != DISP_E_UNKNOWNNAME)
            return hres;
    }

    builtin = find_builtin_prop(This, name, case_insens);
    if(builtin) {
        unsigned flags = builtin->flags;
        if(flags & PROPF_METHOD) {
            jsdisp_t *obj;

            hres = create_builtin_function(This->ctx, builtin->invoke, builtin->name, NULL, flags, NULL, &obj);
            if(FAILED(hres))
                return hres;

            prop = alloc_prop(This, builtin->name, PROP_JSVAL, (flags & PROPF_ALL) | PROPF_WRITABLE | PROPF_CONFIGURABLE);
            if(!prop) {
                jsdisp_release(obj);
                return E_OUTOFMEMORY;
            }

            prop->type = PROP_JSVAL;
            prop->u.val = jsval_obj(obj);
            *ret = prop;
            return S_OK;
        }else if(builtin->setter)
            flags |= PROPF_WRITABLE;
        flags &= PROPF_ENUMERABLE | PROPF_WRITABLE | PROPF_CONFIGURABLE;
        prop = alloc_prop(This, builtin->name, PROP_BUILTIN, flags);
        if(!prop)
            return E_OUTOFMEMORY;

        prop->u.p = builtin;
        *ret = prop;
        return S_OK;
    }

    if(This->builtin_info->idx_length) {
        const WCHAR *ptr;
        unsigned idx = 0;

        for(ptr = name; is_digit(*ptr) && idx < 0x10000; ptr++)
            idx = idx*10 + (*ptr-'0');
        if(!*ptr && idx < This->builtin_info->idx_length(This)) {
            unsigned flags = PROPF_ENUMERABLE;
            if(This->builtin_info->idx_put)
                flags |= PROPF_WRITABLE;
            prop = alloc_prop(This, name, PROP_IDX, flags);
            if(!prop)
                return E_OUTOFMEMORY;

            prop->u.idx = idx;
            *ret = prop;
            return S_OK;
        }
    }

    *ret = NULL;
    return S_OK;
}

static HRESULT find_prop_name_prot(jsdisp_t *This, unsigned hash, const WCHAR *name, BOOL case_insens, dispex_prop_t **ret)
{
    dispex_prop_t *prop, *del=NULL;
    HRESULT hres;

    hres = find_prop_name(This, hash, name, case_insens, &prop);
    if(FAILED(hres))
        return hres;
    if(prop && prop->type==PROP_DELETED) {
        del = prop;
    } else if(prop) {
        fix_protref_prop(This, prop);
        *ret = prop;
        return S_OK;
    }

    if(This->prototype) {
        hres = find_prop_name_prot(This->prototype, hash, name, case_insens, &prop);
        if(FAILED(hres))
            return hres;
        if(prop && prop->type != PROP_DELETED) {
            if(del) {
                del->type = PROP_PROTREF;
                del->u.ref = prop - This->prototype->props;
                prop = del;
            }else {
                prop = alloc_protref(This, prop->name, prop - This->prototype->props);
                if(!prop)
                    return E_OUTOFMEMORY;
            }

            *ret = prop;
            return S_OK;
        }
    }

    *ret = del;
    return S_OK;
}

static HRESULT ensure_prop_name(jsdisp_t *This, const WCHAR *name, DWORD create_flags, BOOL case_insens, dispex_prop_t **ret)
{
    dispex_prop_t *prop;
    HRESULT hres;

    hres = find_prop_name_prot(This, string_hash(name), name, case_insens, &prop);
    if(SUCCEEDED(hres) && (!prop || prop->type == PROP_DELETED)) {
        TRACE("creating prop %s flags %lx\n", debugstr_w(name), create_flags);

        if(prop) {
            prop->type = PROP_JSVAL;
            prop->flags = create_flags;
            prop->u.val = jsval_undefined();
        }else {
            prop = alloc_prop(This, name, PROP_JSVAL, create_flags);
            if(!prop)
                return E_OUTOFMEMORY;
        }

        prop->u.val = jsval_undefined();

        if(This->proxy) {
            struct proxy_prop_info info;

            info.name = name;
            hres = This->proxy->lpVtbl->PropDefineOverride(This->proxy, &info);
            if(hres == S_FALSE)
                hres = S_OK;
            else if(SUCCEEDED(hres))
                hres = alloc_proxy_prop(This, &info, &prop);
        }
    }

    *ret = prop;
    return hres;
}

static HRESULT fix_overridden_prop(jsdisp_t *This, dispex_prop_t *prop)
{
    struct proxy_prop_info info;
    HRESULT hres;

    if(!This->proxy)
        return S_OK;

    info.name = prop->name;
    info.dispid = DISPID_UNKNOWN;

    switch(prop->type) {
    case PROP_PROXY:
        info.dispid = prop->u.proxy_id;
        break;
    case PROP_PROTREF:
    case PROP_DELETED:
        break;
    default:
        return S_OK;
    }

    hres = This->proxy->lpVtbl->PropFixOverride(This->proxy, &info);
    if(hres != S_OK)
        return FAILED(hres) ? hres : S_OK;

    /* Either the prop was restored (to PROP_PROXY), or it was removed */
    if(info.dispid == DISPID_UNKNOWN) {
        if(This->prototype) {
            dispex_prop_t *prot_prop;

            hres = find_prop_name_prot(This->prototype, prop->hash, prop->name, FALSE, &prot_prop);
            if(FAILED(hres))
                return hres;
            if(prot_prop && prot_prop->type != PROP_DELETED) {
                prop->type = PROP_PROTREF;
                prop->u.ref = prot_prop - This->prototype->props;
                return hres;
            }
        }
        prop->type = PROP_DELETED;
    }else {
        info.func[0].invoke = NULL;
        hres = alloc_proxy_prop(This, &info, &prop);
    }

    return hres;
}

static IDispatch *get_this(DISPPARAMS *dp)
{
    DWORD i;

    for(i=0; i < dp->cNamedArgs; i++) {
        if(dp->rgdispidNamedArgs[i] == DISPID_THIS) {
            if(V_VT(dp->rgvarg+i) == VT_DISPATCH)
                return V_DISPATCH(dp->rgvarg+i);

            WARN("This is not VT_DISPATCH\n");
            return NULL;
        }
    }

    TRACE("no this passed\n");
    return NULL;
}

static HRESULT convert_params(script_ctx_t *ctx, const DISPPARAMS *dp, jsval_t *buf, unsigned *argc, jsval_t **ret)
{
    jsval_t *argv;
    unsigned cnt;
    unsigned i;
    HRESULT hres;

    cnt = dp->cArgs - dp->cNamedArgs;

    if(cnt > 6) {
        argv = malloc(cnt * sizeof(*argv));
        if(!argv)
            return E_OUTOFMEMORY;
    }else {
        argv = buf;
    }

    for(i = 0; i < cnt; i++) {
        hres = variant_to_jsval(ctx, dp->rgvarg+dp->cArgs-i-1, argv+i);
        if(FAILED(hres)) {
            while(i--)
                jsval_release(argv[i]);
            if(argv != buf)
                free(argv);
            return hres;
        }
    }

    *argc = cnt;
    *ret = argv;
    return S_OK;
}

static HRESULT proxy_disp_call(jsdisp_t *This, jsval_t vthis, DISPID id, unsigned flags, unsigned argc,
        jsval_t *argv, jsval_t *ret, IServiceProvider *caller)
{
    DISPPARAMS dp = { NULL, NULL, argc, 0 };
    IDispatch *this_obj, *converted = NULL;
    script_ctx_t *ctx = This->ctx;
    EXCEPINFO ei = { 0 };
    VARIANT buf[6], retv;
    jsdisp_t *jsdisp;
    HRESULT hres;
    unsigned i;

    if(!ctx->global)
        return E_UNEXPECTED;

    if(dp.cArgs <= ARRAY_SIZE(buf))
        dp.rgvarg = buf;
    else if(!(dp.rgvarg = malloc(dp.cArgs * sizeof(*dp.rgvarg))))
        return E_OUTOFMEMORY;

    for(i = 0; i < dp.cArgs; i++) {
        hres = jsval_to_variant(argv[i], &dp.rgvarg[dp.cArgs - i - 1]);
        if(FAILED(hres))
            goto cleanup;
    }

    if(is_undefined(vthis) || is_null(vthis))
        this_obj = lookup_global_host(ctx);
    else {
        hres = to_object(ctx, vthis, &converted);
        if(FAILED(hres))
            goto cleanup;
        this_obj = converted;
    }

    jsdisp = to_jsdisp(this_obj);
    if(jsdisp && jsdisp->proxy)
        this_obj = (IDispatch*)jsdisp->proxy;

    V_VT(&retv) = VT_EMPTY;
    flags &= ~DISPATCH_JSCRIPT_INTERNAL_MASK;
    hres = This->proxy->lpVtbl->PropInvoke(This->proxy, this_obj, id, ctx->lcid, flags, &dp, ret ? &retv : NULL, &ei, caller);
    if(converted)
        IDispatch_Release(converted);

    if(hres == DISP_E_EXCEPTION)
        disp_fill_exception(ctx, &ei);
    else if(SUCCEEDED(hres) && ret) {
        hres = variant_to_jsval(ctx, &retv, ret);
        VariantClear(&retv);
    }

cleanup:
    while(i--)
        VariantClear(&dp.rgvarg[i]);
    if(dp.rgvarg != buf)
        free(dp.rgvarg);
    return hres;
}

static HRESULT prop_get(jsdisp_t *This, IDispatch *jsthis, dispex_prop_t *prop, jsval_t *r, IServiceProvider *caller)
{
    jsdisp_t *prop_obj = This;
    HRESULT hres;
    VARIANT var;

    while(prop->type == PROP_PROTREF) {
        prop_obj = prop_obj->prototype;
        prop = prop_obj->props + prop->u.ref;
    }

    if(prop_obj->proxy) {
        hres = prop_obj->proxy->lpVtbl->PropOverride(prop_obj->proxy, prop->name, &var);
        if(hres != S_FALSE) {
            if(SUCCEEDED(hres)) {
                hres = variant_to_jsval(This->ctx, &var, r);
                VariantClear(&var);
            }
            goto done;
        }
    }

    switch(prop->type) {
    case PROP_BUILTIN:
        hres = prop->u.p->getter(This->ctx, prop_obj, r);
        break;
    case PROP_JSVAL:
        hres = jsval_copy(prop->u.val, r);
        break;
    case PROP_ACCESSOR:
        if(prop->u.accessor.getter) {
            hres = jsdisp_call_value(prop->u.accessor.getter, jsval_disp(jsthis), DISPATCH_METHOD, 0, NULL, r, caller);
        }else {
            *r = jsval_undefined();
            hres = S_OK;
        }
        break;
    case PROP_PROXY: {
        DISPPARAMS dp = { 0 };
        EXCEPINFO ei = { 0 };
        jsdisp_t *jsdisp;

        if((jsdisp = to_jsdisp(jsthis)) && jsdisp->proxy)
            jsthis = (IDispatch*)jsdisp->proxy;

        V_VT(&var) = VT_EMPTY;
        hres = prop_obj->proxy->lpVtbl->PropInvoke(prop_obj->proxy, jsthis, prop->u.proxy_id, This->ctx->lcid,
                                                   DISPATCH_PROPERTYGET, &dp, &var, &ei, caller);
        if(hres == DISP_E_EXCEPTION)
            disp_fill_exception(This->ctx, &ei);
        else if(SUCCEEDED(hres)) {
            hres = variant_to_jsval(This->ctx, &var, r);
            VariantClear(&var);
        }
        break;
    }
    case PROP_IDX:
        hres = prop_obj->builtin_info->idx_get(prop_obj, prop->u.idx, r);
        break;
    default:
        ERR("type %d\n", prop->type);
        return E_FAIL;
    }
    if(SUCCEEDED(hres))
        hres = convert_to_proxy(This->ctx, r);

done:
    if(FAILED(hres)) {
        TRACE("fail %08lx\n", hres);
        return hres;
    }

    TRACE("%p.%s ret %s\n", This, debugstr_w(prop->name), debugstr_jsval(*r));
    return hres;
}

static HRESULT prop_put(jsdisp_t *This, dispex_prop_t *prop, jsval_t val, IServiceProvider *caller)
{
    jsdisp_t *prop_obj = This;
    HRESULT hres;

    if(prop->type == PROP_PROTREF) {
        dispex_prop_t *prop_iter = prop;
        jsdisp_t *prototype_iter = This;

        do {
            prototype_iter = prototype_iter->prototype;
            prop_iter = prototype_iter->props + prop_iter->u.ref;
        } while(prop_iter->type == PROP_PROTREF);

        if(prop_iter->type == PROP_ACCESSOR) {
            prop_obj = prototype_iter;
            prop = prop_iter;
        }
    }

    switch(prop->type) {
    case PROP_BUILTIN:
        if(!prop->u.p->setter) {
            TRACE("getter with no setter\n");
            return S_OK;
        }
        return prop->u.p->setter(This->ctx, This, val);
    case PROP_PROTREF:
    case PROP_DELETED:
        if(!This->extensible)
            return S_OK;
        prop->type = PROP_JSVAL;
        prop->flags = PROPF_ENUMERABLE | PROPF_CONFIGURABLE | PROPF_WRITABLE;
        prop->u.val = jsval_undefined();
        break;
    case PROP_JSVAL:
        if(!(prop->flags & PROPF_WRITABLE))
            return S_OK;

        jsval_release(prop->u.val);
        break;
    case PROP_ACCESSOR:
        if(!prop->u.accessor.setter) {
            TRACE("no setter\n");
            return S_OK;
        }
        return jsdisp_call_value(prop->u.accessor.setter, jsval_obj(This), DISPATCH_METHOD, 1, &val, NULL, caller);
    case PROP_PROXY: {
        static DISPID propput_dispid = DISPID_PROPERTYPUT;
        EXCEPINFO ei = { 0 };
        VARIANT var;
        DISPPARAMS dp = { &var, &propput_dispid, 1, 1 };

        if(!(prop->flags & PROPF_WRITABLE))
            return S_OK;
        hres = jsval_to_variant(val, &var);
        if(FAILED(hres))
            return hres;

        hres = prop_obj->proxy->lpVtbl->PropInvoke(prop_obj->proxy, This->proxy ? (IDispatch*)This->proxy : to_disp(This),
                                                   prop->u.proxy_id, This->ctx->lcid, DISPATCH_PROPERTYPUT, &dp, NULL, &ei, caller);
        VariantClear(&var);
        if(hres == S_FALSE) {
            prop->type = PROP_JSVAL;
            prop->flags = PROPF_ENUMERABLE | PROPF_CONFIGURABLE | PROPF_WRITABLE;
            prop->u.val = jsval_undefined();
            break;
        }
        if(hres == DISP_E_EXCEPTION)
            disp_fill_exception(This->ctx, &ei);
        return hres;
    }
    case PROP_IDX:
        if(!This->builtin_info->idx_put) {
            TRACE("no put_idx\n");
            return S_OK;
        }
        return This->builtin_info->idx_put(This, prop->u.idx, val);
    default:
        ERR("type %d\n", prop->type);
        return E_FAIL;
    }

    TRACE("%p.%s = %s\n", This, debugstr_w(prop->name), debugstr_jsval(val));

    hres = jsval_copy(val, &prop->u.val);
    if(FAILED(hres))
        return hres;

    if(This->builtin_info->on_put)
        This->builtin_info->on_put(This, prop->name);

    return S_OK;
}

static HRESULT invoke_prop_func(jsdisp_t *This, IDispatch *jsthis, dispex_prop_t *prop, WORD flags,
        unsigned argc, jsval_t *argv, jsval_t *r, IServiceProvider *caller)
{
    HRESULT hres;

    switch(prop->type) {
    case PROP_BUILTIN:
        return JS_E_FUNCTION_EXPECTED;
    case PROP_PROTREF:
        return invoke_prop_func(This->prototype, jsthis ? jsthis : (IDispatch *)&This->IDispatchEx_iface,
                                This->prototype->props+prop->u.ref, flags, argc, argv, r, caller);
    case PROP_JSVAL: {
        if(!is_object_instance(prop->u.val)) {
            FIXME("invoke %s\n", debugstr_jsval(prop->u.val));
            return E_FAIL;
        }

        TRACE("call %s %p\n", debugstr_w(prop->name), get_object(prop->u.val));

        return disp_call_value_with_caller(This->ctx, get_object(prop->u.val),
                jsval_disp(jsthis ? jsthis : (IDispatch*)&This->IDispatchEx_iface),
                flags, argc, argv, r, caller);
    }
    case PROP_ACCESSOR:
    case PROP_IDX: {
        jsval_t val;

        hres = prop_get(This, jsthis ? jsthis : (IDispatch *)&This->IDispatchEx_iface, prop, &val, caller);
        if(FAILED(hres))
            return hres;

        if(is_object_instance(val)) {
            hres = disp_call_value_with_caller(This->ctx, get_object(val),
                    jsval_disp(jsthis ? jsthis : (IDispatch*)&This->IDispatchEx_iface),
                    flags, argc, argv, r, caller);
        }else {
            FIXME("invoke %s\n", debugstr_jsval(val));
            hres = E_NOTIMPL;
        }

        jsval_release(val);
        return hres;
    }
    case PROP_PROXY:
        return proxy_disp_call(This, jsval_disp(jsthis ? jsthis : (IDispatch*)&This->IDispatchEx_iface),
                               prop->u.proxy_id, flags, argc, argv, r, caller);
    case PROP_DELETED:
        assert(0);
        break;
    }

    return E_FAIL;
}

HRESULT builtin_set_const(script_ctx_t *ctx, jsdisp_t *jsthis, jsval_t value)
{
    TRACE("%p %s\n", jsthis, debugstr_jsval(value));
    return S_OK;
}

static HRESULT fill_props(jsdisp_t *obj)
{
    dispex_prop_t *prop;
    HRESULT hres;

    if(obj->proxy) {
        hres = obj->proxy->lpVtbl->PropEnum(obj->proxy);
        if(FAILED(hres))
            return hres;
    }

    if(obj->builtin_info->idx_length) {
        unsigned i = 0, len = obj->builtin_info->idx_length(obj);
        WCHAR name[12];

        for(i = 0; i < len; i++) {
            swprintf(name, ARRAY_SIZE(name), L"%u", i);
            hres = find_prop_name(obj, string_hash(name), name, FALSE, &prop);
            if(FAILED(hres))
                return hres;
        }
    }

    return S_OK;
}

static HRESULT fill_protrefs(jsdisp_t *This)
{
    dispex_prop_t *iter, *prop;
    unsigned idx;
    HRESULT hres;

    hres = fill_props(This);
    if(FAILED(hres))
        return hres;

    if(!This->prototype)
        return S_OK;

    hres = fill_protrefs(This->prototype);
    if(FAILED(hres))
        return hres;

    for(iter = This->prototype->props; iter < This->prototype->props+This->prototype->prop_cnt; iter++) {
        if(override_idx(This, iter->name, &idx))
            continue;
        hres = find_prop_name(This, iter->hash, iter->name, FALSE, &prop);
        if(FAILED(hres))
            return hres;
        if(!prop || prop->type==PROP_DELETED) {
            if(prop) {
                prop->type = PROP_PROTREF;
                prop->flags = 0;
                prop->u.ref = iter - This->prototype->props;
            }else {
                prop = alloc_protref(This, iter->name, iter - This->prototype->props);
                if(!prop)
                    return E_OUTOFMEMORY;
            }
        }
    }

    return S_OK;
}

static void unlink_jsdisp(jsdisp_t *jsdisp)
{
    dispex_prop_t *prop = jsdisp->props, *end;

    for(end = prop + jsdisp->prop_cnt; prop < end; prop++) {
        switch(prop->type) {
        case PROP_DELETED:
            continue;
        case PROP_JSVAL:
            jsval_release(prop->u.val);
            break;
        case PROP_ACCESSOR:
            if(prop->u.accessor.getter)
                jsdisp_release(prop->u.accessor.getter);
            if(prop->u.accessor.setter)
                jsdisp_release(prop->u.accessor.setter);
            break;
        default:
            break;
        }
        prop->type = PROP_DELETED;
    }

    if(jsdisp->prototype) {
        jsdisp_release(jsdisp->prototype);
        jsdisp->prototype = NULL;
    }

    if(jsdisp->builtin_info->gc_traverse)
        jsdisp->builtin_info->gc_traverse(NULL, GC_TRAVERSE_UNLINK, jsdisp);
}



/*
 * To deal with circular refcounts, a basic Garbage Collector is used with a variant of the
 * mark-and-sweep algorithm that doesn't require knowing or traversing any specific "roots".
 * This works based on the assumption that circular references can only happen when objects
 * end up pointing to each other, and each other alone, without any external refs.
 *
 * An "external ref" is a ref to the object that's not from any other object. Example of such
 * refs can be local variables, the script ctx (which keeps a ref to the global object), etc.
 *
 * At a high level, there are 3 logical passes done on the entire list of objects:
 *
 * 1. Speculatively decrease refcounts of each linked-to-object from each object. This ensures
 *    that the only remaining refcount on each object is the number of "external refs" to it.
 *    At the same time, mark all of the objects so that they can be potentially collected.
 *
 * 2. For each object with a non-zero "external refcount", clear the mark from step 1, and
 *    recursively traverse all linked objects from it, clearing their marks as well (regardless
 *    of their refcount), stopping a given path when the object is unmarked (and then going back
 *    up the GC stack). This basically unmarks all of the objects with "external refcounts"
 *    and those accessible from them, and only the leaked dangling objects will still be marked.
 *
 * 3. For each object that is marked, unlink all of the objects linked from it, because they
 *    are dangling in a circular refcount and not accessible. This should release them.
 *
 * During unlinking (GC_TRAVERSE_UNLINK), it is important that we unlink *all* linked objects
 * from the object, to be certain that releasing the object later will not delete any other
 * objects. Otherwise calculating the "next" object in the list becomes impossible.
 *
 * This collection process has to be done periodically, but can be pretty expensive so there
 * has to be a balance between reclaiming dangling objects and performance.
 *
 */
struct gc_stack_chunk {
    jsdisp_t *objects[1020];
    struct gc_stack_chunk *prev;
};

struct gc_ctx {
    struct gc_stack_chunk *chunk;
    struct gc_stack_chunk *next;
    unsigned idx;
};

static HRESULT gc_stack_push(struct gc_ctx *gc_ctx, jsdisp_t *obj)
{
    if(!gc_ctx->idx) {
        if(gc_ctx->next)
            gc_ctx->chunk = gc_ctx->next;
        else {
            struct gc_stack_chunk *prev, *tmp = malloc(sizeof(*tmp));
            if(!tmp)
                return E_OUTOFMEMORY;
            prev = gc_ctx->chunk;
            gc_ctx->chunk = tmp;
            gc_ctx->chunk->prev = prev;
        }
        gc_ctx->idx = ARRAY_SIZE(gc_ctx->chunk->objects);
        gc_ctx->next = NULL;
    }
    gc_ctx->chunk->objects[--gc_ctx->idx] = obj;
    return S_OK;
}

static jsdisp_t *gc_stack_pop(struct gc_ctx *gc_ctx)
{
    jsdisp_t *obj = gc_ctx->chunk->objects[gc_ctx->idx];

    if(++gc_ctx->idx == ARRAY_SIZE(gc_ctx->chunk->objects)) {
        free(gc_ctx->next);
        gc_ctx->next = gc_ctx->chunk;
        gc_ctx->chunk = gc_ctx->chunk->prev;
        gc_ctx->idx = 0;
    }
    return obj;
}

HRESULT gc_run(script_ctx_t *ctx, BOOL force_cc)
{
    /* Save original refcounts in a linked list of chunks */
    struct chunk
    {
        struct chunk *next;
        LONG ref[1020];
    } *head, *chunk;
    jsdisp_t *obj, *obj2, *link, *link2;
    dispex_prop_t *prop, *props_end;
    struct gc_ctx gc_ctx = { 0 };
    unsigned chunk_idx = 0;
    HRESULT hres = S_OK;
    struct list *iter;

    /* Prevent recursive calls from side-effects during unlinking (e.g. CollectGarbage from host object's Release) */
    if(ctx->gc_is_unlinking)
        return S_OK;

    if(ctx->html_mode && ctx->site) {
        ctx->gc_is_unlinking = TRUE;
        cc_api.collect(ctx->site, force_cc);
        ctx->gc_is_unlinking = FALSE;
    }

    if(!(head = malloc(sizeof(*head))))
        return E_OUTOFMEMORY;
    head->next = NULL;
    chunk = head;

    /* 1. Save actual refcounts and decrease them speculatively as-if we unlinked the objects */
    LIST_FOR_EACH_ENTRY(obj, &ctx->objects, jsdisp_t, entry) {
        if(chunk_idx == ARRAY_SIZE(chunk->ref)) {
            if(!(chunk->next = malloc(sizeof(*chunk)))) {
                do {
                    chunk = head->next;
                    free(head);
                    head = chunk;
                } while(head);
                return E_OUTOFMEMORY;
            }
            chunk = chunk->next, chunk_idx = 0;
            chunk->next = NULL;
        }
        chunk->ref[chunk_idx++] = obj->ref;
    }
    LIST_FOR_EACH_ENTRY(obj, &ctx->objects, jsdisp_t, entry) {
        for(prop = obj->props, props_end = prop + obj->prop_cnt; prop < props_end; prop++) {
            switch(prop->type) {
            case PROP_JSVAL:
                if(is_object_instance(prop->u.val) && (link = to_jsdisp(get_object(prop->u.val))) && link->ctx == ctx)
                    link->ref--;
                break;
            case PROP_ACCESSOR:
                if(prop->u.accessor.getter && prop->u.accessor.getter->ctx == ctx)
                    prop->u.accessor.getter->ref--;
                if(prop->u.accessor.setter && prop->u.accessor.setter->ctx == ctx)
                    prop->u.accessor.setter->ref--;
                break;
            default:
                break;
            }
        }

        if(obj->prototype && obj->prototype->ctx == ctx)
            obj->prototype->ref--;
        if(obj->builtin_info->gc_traverse)
            obj->builtin_info->gc_traverse(&gc_ctx, GC_TRAVERSE_SPECULATIVELY, obj);
        obj->gc_marked = TRUE;
    }

    /* 2. Clear mark on objects with non-zero "external refcount" and all objects accessible from them */
    LIST_FOR_EACH_ENTRY(obj, &ctx->objects, jsdisp_t, entry) {
        if(!obj->gc_marked || (!obj->ref && !obj->proxy))
            continue;

        hres = gc_stack_push(&gc_ctx, NULL);
        if(FAILED(hres))
            break;

        obj2 = obj;
        do
        {
            obj2->gc_marked = FALSE;

            for(prop = obj2->props, props_end = prop + obj2->prop_cnt; prop < props_end; prop++) {
                switch(prop->type) {
                case PROP_JSVAL:
                    if(!is_object_instance(prop->u.val))
                        continue;
                    link = to_jsdisp(get_object(prop->u.val));
                    link2 = NULL;
                    break;
                case PROP_ACCESSOR:
                    link = prop->u.accessor.getter;
                    link2 = prop->u.accessor.setter;
                    break;
                default:
                    continue;
                }
                if(link && link->gc_marked && link->ctx == ctx) {
                    hres = gc_stack_push(&gc_ctx, link);
                    if(FAILED(hres))
                        break;
                }
                if(link2 && link2->gc_marked && link2->ctx == ctx) {
                    hres = gc_stack_push(&gc_ctx, link2);
                    if(FAILED(hres))
                        break;
                }
            }

            if(FAILED(hres))
                break;

            if(obj2->prototype && obj2->prototype->gc_marked && obj2->prototype->ctx == ctx) {
                hres = gc_stack_push(&gc_ctx, obj2->prototype);
                if(FAILED(hres))
                    break;
            }

            if(obj2->builtin_info->gc_traverse) {
                hres = obj2->builtin_info->gc_traverse(&gc_ctx, GC_TRAVERSE, obj2);
                if(FAILED(hres))
                    break;
            }

            /* For weak refs, traverse paths accessible from it via the WeakMaps, if the WeakMaps are alive at this point.
               We need both the key and the WeakMap for the entry to actually be accessible (and thus traversed). */
            if(obj2->has_weak_refs) {
                struct list *list = &RB_ENTRY_VALUE(rb_get(&ctx->weak_refs, obj2), struct weak_refs_entry, entry)->list;
                struct weakmap_entry *entry;

                LIST_FOR_EACH_ENTRY(entry, list, struct weakmap_entry, weak_refs_entry) {
                    if(!entry->weakmap->gc_marked && is_object_instance(entry->value) && (link = to_jsdisp(get_object(entry->value)))) {
                        hres = gc_stack_push(&gc_ctx, link);
                        if(FAILED(hres))
                            break;
                    }
                }

                if(FAILED(hres))
                    break;
            }

            do obj2 = gc_stack_pop(&gc_ctx); while(obj2 && !obj2->gc_marked);
        } while(obj2);

        if(FAILED(hres)) {
            do obj2 = gc_stack_pop(&gc_ctx); while(obj2);
            break;
        }
    }
    free(gc_ctx.next);

    /* Restore */
    chunk = head, chunk_idx = 0;
    LIST_FOR_EACH_ENTRY(obj, &ctx->objects, jsdisp_t, entry) {
        obj->ref = chunk->ref[chunk_idx++];
        if(chunk_idx == ARRAY_SIZE(chunk->ref)) {
            struct chunk *next = chunk->next;
            free(chunk);
            chunk = next, chunk_idx = 0;
        }
    }
    free(chunk);

    if(FAILED(hres))
        return hres;

    /* 3. Remove all the links from the marked objects, since they are dangling */
    ctx->gc_is_unlinking = TRUE;

    iter = list_head(&ctx->objects);
    while(iter) {
        obj = LIST_ENTRY(iter, jsdisp_t, entry);
        if(!obj->gc_marked) {
            iter = list_next(&ctx->objects, iter);
            continue;
        }

        /* Grab it since it gets removed when unlinked */
        jsdisp_addref(obj);
        unlink_jsdisp(obj);

        /* Releasing unlinked object should not delete any other object,
           so we can safely obtain the next pointer now */
        iter = list_next(&ctx->objects, iter);
        jsdisp_release(obj);
    }

    ctx->gc_is_unlinking = FALSE;
    ctx->gc_last_tick = GetTickCount();
    return S_OK;
}

HRESULT gc_process_linked_obj(struct gc_ctx *gc_ctx, enum gc_traverse_op op, jsdisp_t *obj, jsdisp_t *link, void **unlink_ref)
{
    if(op == GC_TRAVERSE_UNLINK) {
        *unlink_ref = NULL;
        jsdisp_release(link);
        return S_OK;
    }

    if(link->ctx != obj->ctx)
        return S_OK;
    if(op == GC_TRAVERSE_SPECULATIVELY)
        link->ref--;
    else if(link->gc_marked)
        return gc_stack_push(gc_ctx, link);
    return S_OK;
}

HRESULT gc_process_linked_val(struct gc_ctx *gc_ctx, enum gc_traverse_op op, jsdisp_t *obj, jsval_t *link)
{
    jsdisp_t *jsdisp;

    if(op == GC_TRAVERSE_UNLINK) {
        jsval_t val = *link;
        *link = jsval_undefined();
        jsval_release(val);
        return S_OK;
    }

    if(!is_object_instance(*link) || !(jsdisp = to_jsdisp(get_object(*link))) || jsdisp->ctx != obj->ctx)
        return S_OK;
    if(op == GC_TRAVERSE_SPECULATIVELY)
        jsdisp->ref--;
    else if(jsdisp->gc_marked)
        return gc_stack_push(gc_ctx, jsdisp);
    return S_OK;
}



struct typeinfo_func {
    dispex_prop_t *prop;
    function_code_t *code;
};

typedef struct {
    ITypeInfo ITypeInfo_iface;
    ITypeComp ITypeComp_iface;
    LONG ref;

    UINT num_funcs;
    UINT num_vars;
    struct typeinfo_func *funcs;
    dispex_prop_t **vars;

    jsdisp_t *jsdisp;
} ScriptTypeInfo;

static struct typeinfo_func *get_func_from_memid(const ScriptTypeInfo *typeinfo, MEMBERID memid)
{
    UINT a = 0, b = typeinfo->num_funcs;

    while (a < b)
    {
        UINT i = (a + b - 1) / 2;
        MEMBERID func_memid = prop_to_id(typeinfo->jsdisp, typeinfo->funcs[i].prop);

        if (memid == func_memid)
            return &typeinfo->funcs[i];
        else if (memid < func_memid)
            b = i;
        else
            a = i + 1;
    }
    return NULL;
}

static dispex_prop_t *get_var_from_memid(const ScriptTypeInfo *typeinfo, MEMBERID memid)
{
    UINT a = 0, b = typeinfo->num_vars;

    while (a < b)
    {
        UINT i = (a + b - 1) / 2;
        MEMBERID var_memid = prop_to_id(typeinfo->jsdisp, typeinfo->vars[i]);

        if (memid == var_memid)
            return typeinfo->vars[i];
        else if (memid < var_memid)
            b = i;
        else
            a = i + 1;
    }
    return NULL;
}

static inline ScriptTypeInfo *ScriptTypeInfo_from_ITypeInfo(ITypeInfo *iface)
{
    return CONTAINING_RECORD(iface, ScriptTypeInfo, ITypeInfo_iface);
}

static inline ScriptTypeInfo *ScriptTypeInfo_from_ITypeComp(ITypeComp *iface)
{
    return CONTAINING_RECORD(iface, ScriptTypeInfo, ITypeComp_iface);
}

static HRESULT WINAPI ScriptTypeInfo_QueryInterface(ITypeInfo *iface, REFIID riid, void **ppv)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    if (IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_ITypeInfo, riid))
        *ppv = &This->ITypeInfo_iface;
    else if (IsEqualGUID(&IID_ITypeComp, riid))
        *ppv = &This->ITypeComp_iface;
    else
    {
        WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI ScriptTypeInfo_AddRef(ITypeInfo *iface)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI ScriptTypeInfo_Release(ITypeInfo *iface)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    UINT i;

    TRACE("(%p) ref=%ld\n", This, ref);

    if (!ref)
    {
        for (i = This->num_funcs; i--;)
            release_bytecode(This->funcs[i].code->bytecode);
        IDispatchEx_Release(&This->jsdisp->IDispatchEx_iface);
        free(This->funcs);
        free(This->vars);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI ScriptTypeInfo_GetTypeAttr(ITypeInfo *iface, TYPEATTR **ppTypeAttr)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    TYPEATTR *attr;

    TRACE("(%p)->(%p)\n", This, ppTypeAttr);

    if (!ppTypeAttr) return E_INVALIDARG;

    attr = calloc(1, sizeof(*attr));
    if (!attr) return E_OUTOFMEMORY;

    attr->guid = GUID_JScriptTypeInfo;
    attr->lcid = LOCALE_USER_DEFAULT;
    attr->memidConstructor = MEMBERID_NIL;
    attr->memidDestructor = MEMBERID_NIL;
    attr->cbSizeInstance = 4;
    attr->typekind = TKIND_DISPATCH;
    attr->cFuncs = This->num_funcs;
    attr->cVars = This->num_vars;
    attr->cImplTypes = 1;
    attr->cbSizeVft = sizeof(IDispatchVtbl);
    attr->cbAlignment = 4;
    attr->wTypeFlags = TYPEFLAG_FDISPATCHABLE;
    attr->wMajorVerNum = JSCRIPT_MAJOR_VERSION;
    attr->wMinorVerNum = JSCRIPT_MINOR_VERSION;

    *ppTypeAttr = attr;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetTypeComp(ITypeInfo *iface, ITypeComp **ppTComp)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%p)\n", This, ppTComp);

    if (!ppTComp) return E_INVALIDARG;

    *ppTComp = &This->ITypeComp_iface;
    ITypeInfo_AddRef(iface);
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetFuncDesc(ITypeInfo *iface, UINT index, FUNCDESC **ppFuncDesc)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    struct typeinfo_func *func;
    FUNCDESC *desc;
    unsigned i;

    TRACE("(%p)->(%u %p)\n", This, index, ppFuncDesc);

    if (!ppFuncDesc) return E_INVALIDARG;
    if (index >= This->num_funcs) return TYPE_E_ELEMENTNOTFOUND;
    func = &This->funcs[index];

    /* Store the parameter array after the FUNCDESC structure */
    desc = calloc(1, sizeof(*desc) + sizeof(ELEMDESC) * func->code->param_cnt);
    if (!desc) return E_OUTOFMEMORY;

    desc->memid = prop_to_id(This->jsdisp, func->prop);
    desc->funckind = FUNC_DISPATCH;
    desc->invkind = INVOKE_FUNC;
    desc->callconv = CC_STDCALL;
    desc->cParams = func->code->param_cnt;
    desc->elemdescFunc.tdesc.vt = VT_VARIANT;

    if (func->code->param_cnt) desc->lprgelemdescParam = (ELEMDESC*)(desc + 1);
    for (i = 0; i < func->code->param_cnt; i++)
        desc->lprgelemdescParam[i].tdesc.vt = VT_VARIANT;

    *ppFuncDesc = desc;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetVarDesc(ITypeInfo *iface, UINT index, VARDESC **ppVarDesc)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    VARDESC *desc;

    TRACE("(%p)->(%u %p)\n", This, index, ppVarDesc);

    if (!ppVarDesc) return E_INVALIDARG;
    if (index >= This->num_vars) return TYPE_E_ELEMENTNOTFOUND;

    desc = calloc(1, sizeof(*desc));
    if (!desc) return E_OUTOFMEMORY;

    desc->memid = prop_to_id(This->jsdisp, This->vars[index]);
    desc->varkind = VAR_DISPATCH;
    desc->elemdescVar.tdesc.vt = VT_VARIANT;

    *ppVarDesc = desc;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetNames(ITypeInfo *iface, MEMBERID memid, BSTR *rgBstrNames,
        UINT cMaxNames, UINT *pcNames)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    struct typeinfo_func *func;
    ITypeInfo *disp_typeinfo;
    dispex_prop_t *var;
    HRESULT hr;
    UINT i = 0;

    TRACE("(%p)->(%ld %p %u %p)\n", This, memid, rgBstrNames, cMaxNames, pcNames);

    if (!rgBstrNames || !pcNames) return E_INVALIDARG;
    if (memid <= 0) return TYPE_E_ELEMENTNOTFOUND;

    func = get_func_from_memid(This, memid);
    if (!func)
    {
        var = get_var_from_memid(This, memid);
        if (!var)
        {
            hr = get_dispatch_typeinfo(&disp_typeinfo);
            if (FAILED(hr)) return hr;

            return ITypeInfo_GetNames(disp_typeinfo, memid, rgBstrNames, cMaxNames, pcNames);
        }
    }

    *pcNames = 0;
    if (!cMaxNames) return S_OK;

    rgBstrNames[0] = SysAllocString(func ? func->prop->name : var->name);
    if (!rgBstrNames[0]) return E_OUTOFMEMORY;
    i++;

    if (func)
    {
        unsigned num = min(cMaxNames, func->code->param_cnt + 1);

        for (; i < num; i++)
        {
            if (!(rgBstrNames[i] = SysAllocString(func->code->params[i - 1])))
            {
                do SysFreeString(rgBstrNames[--i]); while (i);
                return E_OUTOFMEMORY;
            }
        }
    }

    *pcNames = i;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetRefTypeOfImplType(ITypeInfo *iface, UINT index, HREFTYPE *pRefType)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%u %p)\n", This, index, pRefType);

    /* We only inherit from IDispatch */
    if (!pRefType) return E_INVALIDARG;
    if (index != 0) return TYPE_E_ELEMENTNOTFOUND;

    *pRefType = 1;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetImplTypeFlags(ITypeInfo *iface, UINT index, INT *pImplTypeFlags)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%u %p)\n", This, index, pImplTypeFlags);

    if (!pImplTypeFlags) return E_INVALIDARG;
    if (index != 0) return TYPE_E_ELEMENTNOTFOUND;

    *pImplTypeFlags = 0;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetIDsOfNames(ITypeInfo *iface, LPOLESTR *rgszNames, UINT cNames,
        MEMBERID *pMemId)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    ITypeInfo *disp_typeinfo;
    const WCHAR *name;
    HRESULT hr = S_OK;
    int i, j, arg;

    TRACE("(%p)->(%p %u %p)\n", This, rgszNames, cNames, pMemId);

    if (!rgszNames || !cNames || !pMemId) return E_INVALIDARG;

    for (i = 0; i < cNames; i++) pMemId[i] = MEMBERID_NIL;
    name = rgszNames[0];

    for (i = 0; i < This->num_funcs; i++)
    {
        struct typeinfo_func *func = &This->funcs[i];

        if (wcsicmp(name, func->prop->name)) continue;
        pMemId[0] = prop_to_id(This->jsdisp, func->prop);

        for (j = 1; j < cNames; j++)
        {
            name = rgszNames[j];
            for (arg = func->code->param_cnt; --arg >= 0;)
                if (!wcsicmp(name, func->code->params[arg]))
                    break;
            if (arg >= 0)
                pMemId[j] = arg;
            else
                hr = DISP_E_UNKNOWNNAME;
        }
        return hr;
    }

    for (i = 0; i < This->num_vars; i++)
    {
        dispex_prop_t *var = This->vars[i];

        if (wcsicmp(name, var->name)) continue;
        pMemId[0] = prop_to_id(This->jsdisp, var);
        return S_OK;
    }

    /* Look into the inherited IDispatch */
    hr = get_dispatch_typeinfo(&disp_typeinfo);
    if (FAILED(hr)) return hr;

    return ITypeInfo_GetIDsOfNames(disp_typeinfo, rgszNames, cNames, pMemId);
}

static HRESULT WINAPI ScriptTypeInfo_Invoke(ITypeInfo *iface, PVOID pvInstance, MEMBERID memid, WORD wFlags,
        DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    ITypeInfo *disp_typeinfo;
    IDispatch *disp;
    HRESULT hr;

    TRACE("(%p)->(%p %ld %d %p %p %p %p)\n", This, pvInstance, memid, wFlags,
          pDispParams, pVarResult, pExcepInfo, puArgErr);

    if (!pvInstance) return E_INVALIDARG;
    if (memid <= 0) return TYPE_E_ELEMENTNOTFOUND;

    if (!get_func_from_memid(This, memid) && !get_var_from_memid(This, memid))
    {
        hr = get_dispatch_typeinfo(&disp_typeinfo);
        if (FAILED(hr)) return hr;

        return ITypeInfo_Invoke(disp_typeinfo, pvInstance, memid, wFlags, pDispParams,
                                pVarResult, pExcepInfo, puArgErr);
    }

    hr = IUnknown_QueryInterface((IUnknown*)pvInstance, &IID_IDispatch, (void**)&disp);
    if (FAILED(hr)) return hr;

    hr = IDispatch_Invoke(disp, memid, &IID_NULL, LOCALE_USER_DEFAULT, wFlags,
                          pDispParams, pVarResult, pExcepInfo, puArgErr);
    IDispatch_Release(disp);

    return hr;
}

static HRESULT WINAPI ScriptTypeInfo_GetDocumentation(ITypeInfo *iface, MEMBERID memid, BSTR *pBstrName,
        BSTR *pBstrDocString, DWORD *pdwHelpContext, BSTR *pBstrHelpFile)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    struct typeinfo_func *func;
    ITypeInfo *disp_typeinfo;
    dispex_prop_t *var;
    HRESULT hr;

    TRACE("(%p)->(%ld %p %p %p %p)\n", This, memid, pBstrName, pBstrDocString, pdwHelpContext, pBstrHelpFile);

    if (pBstrDocString) *pBstrDocString = NULL;
    if (pdwHelpContext) *pdwHelpContext = 0;
    if (pBstrHelpFile) *pBstrHelpFile = NULL;

    if (memid == MEMBERID_NIL)
    {
        if (pBstrName && !(*pBstrName = SysAllocString(L"JScriptTypeInfo")))
            return E_OUTOFMEMORY;
        if (pBstrDocString &&
            !(*pBstrDocString = SysAllocString(L"JScript Type Info")))
        {
            if (pBstrName) SysFreeString(*pBstrName);
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    if (memid <= 0) return TYPE_E_ELEMENTNOTFOUND;

    func = get_func_from_memid(This, memid);
    if (!func)
    {
        var = get_var_from_memid(This, memid);
        if (!var)
        {
            hr = get_dispatch_typeinfo(&disp_typeinfo);
            if (FAILED(hr)) return hr;

            return ITypeInfo_GetDocumentation(disp_typeinfo, memid, pBstrName, pBstrDocString,
                                              pdwHelpContext, pBstrHelpFile);
        }
    }

    if (pBstrName)
    {
        *pBstrName = SysAllocString(func ? func->prop->name : var->name);

        if (!*pBstrName)
            return E_OUTOFMEMORY;
    }
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetDllEntry(ITypeInfo *iface, MEMBERID memid, INVOKEKIND invKind,
        BSTR *pBstrDllName, BSTR *pBstrName, WORD *pwOrdinal)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    ITypeInfo *disp_typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%ld %d %p %p %p)\n", This, memid, invKind, pBstrDllName, pBstrName, pwOrdinal);

    if (pBstrDllName) *pBstrDllName = NULL;
    if (pBstrName) *pBstrName = NULL;
    if (pwOrdinal) *pwOrdinal = 0;

    if (!get_func_from_memid(This, memid) && !get_var_from_memid(This, memid))
    {
        hr = get_dispatch_typeinfo(&disp_typeinfo);
        if (FAILED(hr)) return hr;

        return ITypeInfo_GetDllEntry(disp_typeinfo, memid, invKind, pBstrDllName, pBstrName, pwOrdinal);
    }
    return TYPE_E_BADMODULEKIND;
}

static HRESULT WINAPI ScriptTypeInfo_GetRefTypeInfo(ITypeInfo *iface, HREFTYPE hRefType, ITypeInfo **ppTInfo)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    HRESULT hr;

    TRACE("(%p)->(%lx %p)\n", This, hRefType, ppTInfo);

    if (!ppTInfo || (INT)hRefType < 0) return E_INVALIDARG;

    if (hRefType & ~3) return E_FAIL;
    if (hRefType & 1)
    {
        hr = get_dispatch_typeinfo(ppTInfo);
        if (FAILED(hr)) return hr;
    }
    else
        *ppTInfo = iface;

    ITypeInfo_AddRef(*ppTInfo);
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_AddressOfMember(ITypeInfo *iface, MEMBERID memid, INVOKEKIND invKind, PVOID *ppv)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    ITypeInfo *disp_typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%ld %d %p)\n", This, memid, invKind, ppv);

    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;

    if (!get_func_from_memid(This, memid) && !get_var_from_memid(This, memid))
    {
        hr = get_dispatch_typeinfo(&disp_typeinfo);
        if (FAILED(hr)) return hr;

        return ITypeInfo_AddressOfMember(disp_typeinfo, memid, invKind, ppv);
    }
    return TYPE_E_BADMODULEKIND;
}

static HRESULT WINAPI ScriptTypeInfo_CreateInstance(ITypeInfo *iface, IUnknown *pUnkOuter, REFIID riid, PVOID *ppvObj)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%p %s %p)\n", This, pUnkOuter, debugstr_guid(riid), ppvObj);

    if (!ppvObj) return E_INVALIDARG;

    *ppvObj = NULL;
    return TYPE_E_BADMODULEKIND;
}

static HRESULT WINAPI ScriptTypeInfo_GetMops(ITypeInfo *iface, MEMBERID memid, BSTR *pBstrMops)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);
    ITypeInfo *disp_typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%ld %p)\n", This, memid, pBstrMops);

    if (!pBstrMops) return E_INVALIDARG;

    if (!get_func_from_memid(This, memid) && !get_var_from_memid(This, memid))
    {
        hr = get_dispatch_typeinfo(&disp_typeinfo);
        if (FAILED(hr)) return hr;

        return ITypeInfo_GetMops(disp_typeinfo, memid, pBstrMops);
    }

    *pBstrMops = NULL;
    return S_OK;
}

static HRESULT WINAPI ScriptTypeInfo_GetContainingTypeLib(ITypeInfo *iface, ITypeLib **ppTLib, UINT *pIndex)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    FIXME("(%p)->(%p %p)\n", This, ppTLib, pIndex);

    return E_NOTIMPL;
}

static void WINAPI ScriptTypeInfo_ReleaseTypeAttr(ITypeInfo *iface, TYPEATTR *pTypeAttr)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%p)\n", This, pTypeAttr);

    free(pTypeAttr);
}

static void WINAPI ScriptTypeInfo_ReleaseFuncDesc(ITypeInfo *iface, FUNCDESC *pFuncDesc)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%p)\n", This, pFuncDesc);

    free(pFuncDesc);
}

static void WINAPI ScriptTypeInfo_ReleaseVarDesc(ITypeInfo *iface, VARDESC *pVarDesc)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeInfo(iface);

    TRACE("(%p)->(%p)\n", This, pVarDesc);

    free(pVarDesc);
}

static const ITypeInfoVtbl ScriptTypeInfoVtbl = {
    ScriptTypeInfo_QueryInterface,
    ScriptTypeInfo_AddRef,
    ScriptTypeInfo_Release,
    ScriptTypeInfo_GetTypeAttr,
    ScriptTypeInfo_GetTypeComp,
    ScriptTypeInfo_GetFuncDesc,
    ScriptTypeInfo_GetVarDesc,
    ScriptTypeInfo_GetNames,
    ScriptTypeInfo_GetRefTypeOfImplType,
    ScriptTypeInfo_GetImplTypeFlags,
    ScriptTypeInfo_GetIDsOfNames,
    ScriptTypeInfo_Invoke,
    ScriptTypeInfo_GetDocumentation,
    ScriptTypeInfo_GetDllEntry,
    ScriptTypeInfo_GetRefTypeInfo,
    ScriptTypeInfo_AddressOfMember,
    ScriptTypeInfo_CreateInstance,
    ScriptTypeInfo_GetMops,
    ScriptTypeInfo_GetContainingTypeLib,
    ScriptTypeInfo_ReleaseTypeAttr,
    ScriptTypeInfo_ReleaseFuncDesc,
    ScriptTypeInfo_ReleaseVarDesc
};

static HRESULT WINAPI ScriptTypeComp_QueryInterface(ITypeComp *iface, REFIID riid, void **ppv)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeComp(iface);
    return ITypeInfo_QueryInterface(&This->ITypeInfo_iface, riid, ppv);
}

static ULONG WINAPI ScriptTypeComp_AddRef(ITypeComp *iface)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeComp(iface);
    return ITypeInfo_AddRef(&This->ITypeInfo_iface);
}

static ULONG WINAPI ScriptTypeComp_Release(ITypeComp *iface)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeComp(iface);
    return ITypeInfo_Release(&This->ITypeInfo_iface);
}

static HRESULT WINAPI ScriptTypeComp_Bind(ITypeComp *iface, LPOLESTR szName, ULONG lHashVal, WORD wFlags,
        ITypeInfo **ppTInfo, DESCKIND *pDescKind, BINDPTR *pBindPtr)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeComp(iface);
    UINT flags = wFlags ? wFlags : ~0;
    ITypeInfo *disp_typeinfo;
    ITypeComp *disp_typecomp;
    HRESULT hr;
    UINT i;

    TRACE("(%p)->(%s %08lx %d %p %p %p)\n", This, debugstr_w(szName), lHashVal,
          wFlags, ppTInfo, pDescKind, pBindPtr);

    if (!szName || !ppTInfo || !pDescKind || !pBindPtr)
        return E_INVALIDARG;

    for (i = 0; i < This->num_funcs; i++)
    {
        if (wcsicmp(szName, This->funcs[i].prop->name)) continue;
        if (!(flags & INVOKE_FUNC)) return TYPE_E_TYPEMISMATCH;

        hr = ITypeInfo_GetFuncDesc(&This->ITypeInfo_iface, i, &pBindPtr->lpfuncdesc);
        if (FAILED(hr)) return hr;

        *pDescKind = DESCKIND_FUNCDESC;
        *ppTInfo = &This->ITypeInfo_iface;
        ITypeInfo_AddRef(*ppTInfo);
        return S_OK;
    }

    for (i = 0; i < This->num_vars; i++)
    {
        if (wcsicmp(szName, This->vars[i]->name)) continue;
        if (!(flags & INVOKE_PROPERTYGET)) return TYPE_E_TYPEMISMATCH;

        hr = ITypeInfo_GetVarDesc(&This->ITypeInfo_iface, i, &pBindPtr->lpvardesc);
        if (FAILED(hr)) return hr;

        *pDescKind = DESCKIND_VARDESC;
        *ppTInfo = &This->ITypeInfo_iface;
        ITypeInfo_AddRef(*ppTInfo);
        return S_OK;
    }

    /* Look into the inherited IDispatch */
    hr = get_dispatch_typeinfo(&disp_typeinfo);
    if (FAILED(hr)) return hr;

    hr = ITypeInfo_GetTypeComp(disp_typeinfo, &disp_typecomp);
    if (FAILED(hr)) return hr;

    hr = ITypeComp_Bind(disp_typecomp, szName, lHashVal, wFlags, ppTInfo, pDescKind, pBindPtr);
    ITypeComp_Release(disp_typecomp);
    return hr;
}

static HRESULT WINAPI ScriptTypeComp_BindType(ITypeComp *iface, LPOLESTR szName, ULONG lHashVal,
        ITypeInfo **ppTInfo, ITypeComp **ppTComp)
{
    ScriptTypeInfo *This = ScriptTypeInfo_from_ITypeComp(iface);
    ITypeInfo *disp_typeinfo;
    ITypeComp *disp_typecomp;
    HRESULT hr;

    TRACE("(%p)->(%s %08lx %p %p)\n", This, debugstr_w(szName), lHashVal, ppTInfo, ppTComp);

    if (!szName || !ppTInfo || !ppTComp)
        return E_INVALIDARG;

    /* Look into the inherited IDispatch */
    hr = get_dispatch_typeinfo(&disp_typeinfo);
    if (FAILED(hr)) return hr;

    hr = ITypeInfo_GetTypeComp(disp_typeinfo, &disp_typecomp);
    if (FAILED(hr)) return hr;

    hr = ITypeComp_BindType(disp_typecomp, szName, lHashVal, ppTInfo, ppTComp);
    ITypeComp_Release(disp_typecomp);
    return hr;
}

static const ITypeCompVtbl ScriptTypeCompVtbl = {
    ScriptTypeComp_QueryInterface,
    ScriptTypeComp_AddRef,
    ScriptTypeComp_Release,
    ScriptTypeComp_Bind,
    ScriptTypeComp_BindType
};

static inline jsdisp_t *impl_from_IDispatchEx(IDispatchEx *iface)
{
    return CONTAINING_RECORD(iface, jsdisp_t, IDispatchEx_iface);
}

static HRESULT WINAPI DispatchEx_QueryInterface(IDispatchEx *iface, REFIID riid, void **ppv)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = &This->IDispatchEx_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        TRACE("(%p)->(IID_IDispatch %p)\n", This, ppv);
        *ppv = &This->IDispatchEx_iface;
    }else if(IsEqualGUID(&IID_IDispatchEx, riid)) {
        TRACE("(%p)->(IID_IDispatchEx %p)\n", This, ppv);
        *ppv = &This->IDispatchEx_iface;
    }else if(IsEqualGUID(&IID_nsXPCOMCycleCollectionParticipant, riid)) {
        /* Only expose these during a full CC, as we can't have their refs change between incremental CC phases */
        if(This->ctx->html_mode && cc_api.is_full_cc()) {
            *ppv = &cc_api.participant;
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }else if(IsEqualGUID(&IID_nsCycleCollectionISupports, riid)) {
        if(This->ctx->html_mode && cc_api.is_full_cc()) {
            *ppv = &This->IDispatchEx_iface;
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }else {
        WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    jsdisp_addref(This);
    return S_OK;
}

static ULONG WINAPI DispatchEx_AddRef(IDispatchEx *iface)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    jsdisp_addref(This);
    return This->ref;
}

static ULONG WINAPI DispatchEx_Release(IDispatchEx *iface)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    ULONG ref = --This->ref;
    TRACE("(%p) ref=%ld\n", This, ref);
    if(!ref)
        jsdisp_free(This);
    return ref;
}

static HRESULT WINAPI DispatchEx_GetTypeInfoCount(IDispatchEx *iface, UINT *pctinfo)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%p)\n", This, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetTypeInfo(IDispatchEx *iface, UINT iTInfo, LCID lcid,
                                              ITypeInfo **ppTInfo)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    dispex_prop_t *prop, *cur, *end, **typevar;
    UINT num_funcs = 0, num_vars = 0;
    struct typeinfo_func *typefunc;
    function_code_t *func_code;
    ScriptTypeInfo *typeinfo;
    unsigned pos;

    TRACE("(%p)->(%u %lu %p)\n", This, iTInfo, lcid, ppTInfo);

    if (iTInfo != 0) return DISP_E_BADINDEX;

    for (prop = This->props, end = prop + This->prop_cnt; prop != end; prop++)
    {
        if (prop->type != PROP_JSVAL || !(prop->flags & PROPF_ENUMERABLE))
            continue;

        /* If two identifiers differ only by case, the TypeInfo fails */
        pos = This->props[get_props_idx(This, prop->hash)].bucket_head;
        while (pos != ~0)
        {
            cur = This->props + pos;

            if (prop->hash == cur->hash && prop != cur &&
                cur->type == PROP_JSVAL && (cur->flags & PROPF_ENUMERABLE) &&
                !wcsicmp(prop->name, cur->name))
            {
                return TYPE_E_AMBIGUOUSNAME;
            }
            pos = cur->bucket_next;
        }

        if (is_function_prop(prop))
        {
            if (Function_get_code(as_jsdisp(get_object(prop->u.val))))
                num_funcs++;
        }
        else num_vars++;
    }

    if (!(typeinfo = malloc(sizeof(*typeinfo))))
        return E_OUTOFMEMORY;

    typeinfo->ITypeInfo_iface.lpVtbl = &ScriptTypeInfoVtbl;
    typeinfo->ITypeComp_iface.lpVtbl = &ScriptTypeCompVtbl;
    typeinfo->ref = 1;
    typeinfo->num_vars = num_vars;
    typeinfo->num_funcs = num_funcs;
    typeinfo->jsdisp = This;

    typeinfo->funcs = malloc(sizeof(*typeinfo->funcs) * num_funcs);
    if (!typeinfo->funcs)
    {
        free(typeinfo);
        return E_OUTOFMEMORY;
    }

    typeinfo->vars = malloc(sizeof(*typeinfo->vars) * num_vars);
    if (!typeinfo->vars)
    {
        free(typeinfo->funcs);
        free(typeinfo);
        return E_OUTOFMEMORY;
    }

    typefunc = typeinfo->funcs;
    typevar = typeinfo->vars;
    for (prop = This->props; prop != end; prop++)
    {
        if (prop->type != PROP_JSVAL || !(prop->flags & PROPF_ENUMERABLE))
            continue;

        if (is_function_prop(prop))
        {
            func_code = Function_get_code(as_jsdisp(get_object(prop->u.val)));
            if (!func_code) continue;

            typefunc->prop = prop;
            typefunc->code = func_code;
            typefunc++;

            /* The function may be deleted, so keep a ref */
            bytecode_addref(func_code->bytecode);
        }
        else
            *typevar++ = prop;
    }

    /* Keep a ref to the props and their names */
    IDispatchEx_AddRef(&This->IDispatchEx_iface);

    *ppTInfo = &typeinfo->ITypeInfo_iface;
    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetIDsOfNames(IDispatchEx *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames, LCID lcid,
                                                DISPID *rgDispId)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    UINT i;
    HRESULT hres;

    TRACE("(%p)->(%s %p %u %lu %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    if(cNames == 0)
        return S_OK;

    hres = jsdisp_get_id(This, rgszNames[0], This->proxy ? fdexNameCaseInsensitive : 0, rgDispId);
    if(FAILED(hres))
        return hres;

    /* DISPIDs for parameters don't seem to be supported */
    if(cNames > 1) {
        for(i = 1; i < cNames; i++)
            rgDispId[i] = DISPID_UNKNOWN;
        hres = DISP_E_UNKNOWNNAME;
    }

    return hres;
}

static HRESULT WINAPI DispatchEx_Invoke(IDispatchEx *iface, DISPID dispIdMember,
                                        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%ld %s %ld %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    return IDispatchEx_InvokeEx(&This->IDispatchEx_iface, dispIdMember, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, NULL);
}

static HRESULT WINAPI DispatchEx_GetDispID(IDispatchEx *iface, BSTR bstrName, DWORD grfdex, DISPID *pid)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%s %lx %p)\n", This, debugstr_w(bstrName), grfdex, pid);

    if(grfdex & ~(fdexNameCaseSensitive|fdexNameCaseInsensitive|fdexNameEnsure|fdexNameImplicit|FDEX_VERSION_MASK)) {
        FIXME("Unsupported grfdex %lx\n", grfdex);
        return E_NOTIMPL;
    }

    return jsdisp_get_id(This, bstrName, grfdex, pid);
}

static HRESULT WINAPI DispatchEx_InvokeEx(IDispatchEx *iface, DISPID id, LCID lcid, WORD wFlags, DISPPARAMS *pdp,
        VARIANT *pvarRes, EXCEPINFO *pei, IServiceProvider *pspCaller)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    IServiceProvider *prev_caller;
    dispex_prop_t *prop;
    jsexcept_t ei;
    HRESULT hres;

    TRACE("(%p)->(%lx %lx %x %p %p %p %p)\n", This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);

    if(pvarRes)
        V_VT(pvarRes) = VT_EMPTY;

    prop = get_prop(This, id);
    if(!prop && id != DISPID_VALUE) {
        TRACE("invalid id\n");
        return DISP_E_MEMBERNOTFOUND;
    }

    enter_script(This->ctx, &ei);

    prev_caller = This->ctx->jscaller->caller;
    This->ctx->jscaller->caller = pspCaller;
    if(pspCaller)
        IServiceProvider_AddRef(pspCaller);

    switch(wFlags) {
    case DISPATCH_METHOD|DISPATCH_PROPERTYGET:
        wFlags = DISPATCH_METHOD;
        /* fall through */
    case DISPATCH_METHOD:
    case DISPATCH_CONSTRUCT: {
        jsval_t *argv, buf[6], r;
        IDispatch *passed_this;
        unsigned argc;

        hres = convert_params(This->ctx, pdp, buf, &argc, &argv);
        if(FAILED(hres))
            break;

        passed_this = get_this(pdp);
        if(prop)
            hres = invoke_prop_func(This, passed_this, prop, wFlags, argc, argv, pvarRes ? &r : NULL, pspCaller);
        else
            hres = jsdisp_call_value(This, passed_this ? jsval_disp(passed_this) : jsval_undefined(),
                                     wFlags, argc, argv, pvarRes ? &r : NULL, pspCaller);

        while(argc--)
            jsval_release(argv[argc]);
        if(argv != buf)
            free(argv);
        if(SUCCEEDED(hres) && pvarRes) {
            hres = jsval_to_variant(r, pvarRes);
            jsval_release(r);
        }
        break;
    }
    case DISPATCH_PROPERTYGET: {
        jsval_t r;

        if(prop)
            hres = prop_get(This, to_disp(This), prop, &r, pspCaller);
        else {
            hres = to_primitive(This->ctx, jsval_obj(This), &r, NO_HINT);
            if(hres == JS_E_TO_PRIMITIVE)
                hres = DISP_E_MEMBERNOTFOUND;
        }

        if(SUCCEEDED(hres)) {
            hres = jsval_to_variant(r, pvarRes);
            jsval_release(r);
        }
        break;
    }
    case DISPATCH_PROPERTYPUTREF | DISPATCH_PROPERTYPUT:
    case DISPATCH_PROPERTYPUTREF:
    case DISPATCH_PROPERTYPUT: {
        jsval_t val;
        DWORD i;

        if(!prop) {
            hres = DISP_E_MEMBERNOTFOUND;
            break;
        }

        for(i=0; i < pdp->cNamedArgs; i++) {
            if(pdp->rgdispidNamedArgs[i] == DISPID_PROPERTYPUT)
                break;
        }

        if(i == pdp->cNamedArgs) {
            TRACE("no value to set\n");
            hres = DISP_E_PARAMNOTOPTIONAL;
            break;
        }

        hres = variant_to_jsval(This->ctx, pdp->rgvarg+i, &val);
        if(FAILED(hres))
            break;

        hres = prop_put(This, prop, val, pspCaller);
        jsval_release(val);
        break;
    }
    default:
        FIXME("Unimplemented flags %x\n", wFlags);
        hres = E_INVALIDARG;
        break;
    }

    This->ctx->jscaller->caller = prev_caller;
    if(pspCaller)
        IServiceProvider_Release(pspCaller);
    return leave_script(This->ctx, hres);
}

static HRESULT delete_prop(jsdisp_t *prop_obj, dispex_prop_t *prop, BOOL *ret)
{
    if(prop->type == PROP_PROTREF) {
        *ret = TRUE;
        return S_OK;
    }

    if(prop_obj->proxy) {
        HRESULT hres = prop_obj->proxy->lpVtbl->PropOverride(prop_obj->proxy, prop->name, NULL);
        if(hres != S_FALSE) {
            *ret = TRUE;
            return hres;
        }
    }

    if(!(prop->flags & PROPF_CONFIGURABLE)) {
        *ret = FALSE;
        return S_OK;
    }

    *ret = TRUE;

    if(prop->type == PROP_PROXY) {
        HRESULT hres = prop_obj->proxy->lpVtbl->PropDelete(prop_obj->proxy, prop->u.proxy_id);
        if(SUCCEEDED(hres))
            prop->type = PROP_DELETED;
        return hres;
    }
    if(prop->type == PROP_JSVAL)
        jsval_release(prop->u.val);
    if(prop->type == PROP_ACCESSOR) {
        if(prop->u.accessor.getter)
            jsdisp_release(prop->u.accessor.getter);
        if(prop->u.accessor.setter)
            jsdisp_release(prop->u.accessor.setter);
    }
    prop->type = PROP_DELETED;
    return S_OK;
}

static HRESULT WINAPI DispatchEx_DeleteMemberByName(IDispatchEx *iface, BSTR bstrName, DWORD grfdex)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    dispex_prop_t *prop;
    unsigned idx;
    BOOL b;
    HRESULT hres;

    TRACE("(%p)->(%s %lx)\n", This, debugstr_w(bstrName), grfdex);

    if(grfdex & ~(fdexNameCaseSensitive|fdexNameCaseInsensitive|fdexNameEnsure|fdexNameImplicit|FDEX_VERSION_MASK))
        FIXME("Unsupported grfdex %lx\n", grfdex);

    if(override_idx(This, bstrName, &idx))
        return S_OK;

    hres = find_prop_name(This, string_hash(bstrName), bstrName, grfdex & fdexNameCaseInsensitive, &prop);
    if(FAILED(hres))
        return hres;
    if(!prop) {
        TRACE("not found\n");
        return S_OK;
    }

    return delete_prop(This, prop, &b);
}

static HRESULT WINAPI DispatchEx_DeleteMemberByDispID(IDispatchEx *iface, DISPID id)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    dispex_prop_t *prop;
    BOOL b;

    TRACE("(%p)->(%lx)\n", This, id);

    prop = get_prop(This, id);
    if(!prop) {
        WARN("invalid id\n");
        return DISP_E_MEMBERNOTFOUND;
    }

    return delete_prop(This, prop, &b);
}

static HRESULT WINAPI DispatchEx_GetMemberProperties(IDispatchEx *iface, DISPID id, DWORD grfdexFetch, DWORD *pgrfdex)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    FIXME("(%p)->(%lx %lx %p)\n", This, id, grfdexFetch, pgrfdex);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetMemberName(IDispatchEx *iface, DISPID id, BSTR *pbstrName)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    dispex_prop_t *prop;

    TRACE("(%p)->(%lx %p)\n", This, id, pbstrName);

    prop = get_prop(This, id);
    if(!prop)
        return DISP_E_MEMBERNOTFOUND;

    *pbstrName = SysAllocString(prop->name);
    if(!*pbstrName)
        return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetNextDispID(IDispatchEx *iface, DWORD grfdex, DISPID id, DISPID *pid)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    HRESULT hres = S_FALSE;

    TRACE("(%p)->(%lx %lx %p)\n", This, grfdex, id, pid);

    if(id != DISPID_VALUE)
        hres = jsdisp_next_prop(This, id, JSDISP_ENUM_ALL, pid);
    if(hres == S_FALSE)
        *pid = DISPID_STARTENUM;
    return hres;
}

static HRESULT WINAPI DispatchEx_GetNameSpaceParent(IDispatchEx *iface, IUnknown **ppunk)
{
    jsdisp_t *This = impl_from_IDispatchEx(iface);
    FIXME("(%p)->(%p)\n", This, ppunk);
    return E_NOTIMPL;
}

/* ECMA-262 5.1 Edition    15.1 */
static HRESULT set_js_globals(jsdisp_t *obj)
{
    jsdisp_t *js_global = obj->ctx->js_global;
    const builtin_prop_t *bprop, *bend;
    dispex_prop_t *prop, *end, *dst;
    HRESULT hres;
    BOOL b;

    /* Reset builtins first */
    obj->builtin_info = js_global->builtin_info;
    for(bprop = obj->builtin_info->props, bend = bprop + obj->builtin_info->props_cnt; bprop != bend; bprop++) {
        unsigned hash = string_hash(bprop->name);
        if(!(prop = find_prop_name_raw(obj, hash, bprop->name, FALSE)) || prop->type == PROP_BUILTIN)
            continue;
        if(bprop->flags & PROPF_METHOD) {
            /* Make sure the builtin method is created as a function so it gets copied later */
            hres = find_prop_name(js_global, hash, bprop->name, FALSE, &prop);
            if(FAILED(hres))
                return hres;
        }else {
            prop->flags |= PROPF_CONFIGURABLE;
            delete_prop(obj, prop, &b);
            prop->flags = (bprop->flags & PROPF_ALL) | (bprop->setter ? PROPF_WRITABLE : 0);
            prop->type = PROP_BUILTIN;
            prop->u.p = bprop;
        }
    }

    /* Copy the rest of the props */
    for(prop = js_global->props, end = prop + js_global->prop_cnt; prop != end; prop++) {
        if(prop->type != PROP_JSVAL && prop->type != PROP_ACCESSOR)
            continue;

        /* Alloc it ourselves so we don't look into proxy props when defining it */
        if(!(dst = find_prop_name_raw(obj, prop->hash, prop->name, FALSE))) {
            if(!(dst = alloc_prop(obj, prop->name, PROP_DELETED, 0)))
                return E_OUTOFMEMORY;
        }else {
            dst->flags |= PROPF_CONFIGURABLE;
            delete_prop(obj, dst, &b);
        }

        dst->flags = prop->flags;
        dst->type = prop->type;
        if(prop->type == PROP_JSVAL) {
            hres = jsval_copy(prop->u.val, &dst->u.val);
            if(FAILED(hres))
                return hres;
        }else {
            dst->u.accessor.getter = prop->u.accessor.getter ? jsdisp_addref(prop->u.accessor.getter) : NULL;
            dst->u.accessor.setter = prop->u.accessor.setter ? jsdisp_addref(prop->u.accessor.setter) : NULL;
        }
    }

    return S_OK;
}

static HRESULT get_proxy_default_prototype(script_ctx_t *ctx, IWineDispatchProxyPrivate *proxy, jsdisp_t **prot)
{
    IDispatch *disp = proxy->lpVtbl->GetDefaultPrototype(proxy, ctx->global->proxy);
    HRESULT hres;

    if(!disp)
        return E_OUTOFMEMORY;

    if(disp == WINE_DISP_PROXY_NULL_PROTOTYPE)
        *prot = NULL;
    else if(disp == WINE_DISP_PROXY_OBJECT_PROTOTYPE)
        *prot = jsdisp_addref(ctx->object_prototype);
    else {
        jsval_t tmp = jsval_disp(disp);
        hres = convert_to_proxy(ctx, &tmp);
        if(FAILED(hres))
            return hres;
        *prot = as_jsdisp(get_object(tmp));
    }
    return S_OK;
}

static HRESULT get_proxy_default_constructor(script_ctx_t *ctx, jsdisp_t *jsdisp, jsdisp_t **ctor)
{
    jsdisp_t *old_global = ctx->global;
    IDispatch *disp;
    HRESULT hres;
    jsval_t tmp;

    /* This may end up in CreateConstructor from a nested GetDefaultConstructor via some
     * prototype's setup, if we're actually calling it on the window, which would define
     * all of the constructors, and that assumes the global to be the one required. But
     * in this case we haven't even finished setting up the window, so set it temporarily. */
    ctx->global = jsdisp;
    hres = jsdisp->proxy->lpVtbl->GetDefaultConstructor(jsdisp->proxy, old_global->proxy, &disp);
    ctx->global = old_global;
    if(FAILED(hres))
        return hres;

    if(!disp) {
        /* Set the globals on it if we're the window itself (not the prototype).
         * We also have to set the props on the constructor and prototype, because
         * we had to skip them earlier, since our window was *not* set up yet... */
        if(hres == S_FALSE) {
            hres = set_js_globals(jsdisp);
            if(SUCCEEDED(hres)) {
                dispex_prop_t *ctor_prop = find_prop_name_raw(jsdisp, string_hash(L"Window"), L"Window", FALSE);
                jsdisp_t *ctor_obj;

                if(ctor_prop && ctor_prop->type == PROP_JSVAL && is_object_instance(ctor_prop->u.val) &&
                   (ctor_obj = to_jsdisp(get_object(ctor_prop->u.val))))
                {
                    hres = jsdisp_define_data_property(ctor_obj, L"prototype", 0, jsval_obj(jsdisp->prototype));
                    if(SUCCEEDED(hres))
                        hres = jsdisp_define_data_property(jsdisp->prototype, L"constructor", PROPF_WRITABLE | PROPF_CONFIGURABLE, jsval_obj(ctor_obj));
                }
            }
        }
        *ctor = NULL;
        return hres;
    }

    tmp = jsval_disp(disp);
    hres = convert_to_proxy(ctx, &tmp);
    if(FAILED(hres))
        return hres;
    *ctor = as_jsdisp(get_object(tmp));

    hres = jsdisp_define_data_property(*ctor, L"prototype", 0, jsval_obj(jsdisp));
    if(FAILED(hres))
        jsdisp_release(*ctor);
    return hres;
}

static inline jsdisp_t *impl_from_IWineDispatchProxyCbPrivate(IWineDispatchProxyCbPrivate *iface)
{
    return impl_from_IDispatchEx((IDispatchEx*)iface);
}

static HRESULT WINAPI WineDispatchProxyCbPrivate_InitProxy(IWineDispatchProxyCbPrivate *iface, IDispatch *obj)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    script_ctx_t *ctx = This->ctx;
    jsval_t val = jsval_disp(obj);
    HRESULT hres;

    if(!ctx->global)
        return E_UNEXPECTED;  /* Let caller know it has to initialize the host */

    IDispatch_AddRef(obj);
    hres = convert_to_proxy(ctx, &val);
    if(SUCCEEDED(hres))
        jsval_release(val);
    return hres;
}

static void WINAPI WineDispatchProxyCbPrivate_Unlinked(IWineDispatchProxyCbPrivate *iface, BOOL persist)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);

    if(!persist) {
        IWineDispatchProxyPrivate *proxy = This->proxy;

        This->proxy = NULL;
        if(!This->ref) {
            jsdisp_free(This);
            return;
        }

        /* We hold a ref only when we're not dangling */
        IDispatchEx_Release((IDispatchEx*)proxy);
    }
    unlink_jsdisp(This);
}

static HRESULT WINAPI WineDispatchProxyCbPrivate_HostUpdated(IWineDispatchProxyCbPrivate *iface, IActiveScript *script)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    script_ctx_t *ctx = get_script_ctx(script);
    dispex_prop_t *prop, *end;
    jsdisp_t *prot, *ctor;
    HRESULT hres;
    BOOL b;

    if(!ctx || !ctx->global)
        return S_OK;

    if(This->ctx != ctx) {
        if(ctx->version < SCRIPTLANGUAGEVERSION_ES5) {
            /* Incompatible compat mode, so unlink the proxy */
            *This->proxy->lpVtbl->GetProxyFieldRef(This->proxy) = NULL;
            iface->lpVtbl->Unlinked(iface, FALSE);
            return S_OK;
        }

        hres = get_proxy_default_prototype(ctx, This->proxy, &prot);
        if(FAILED(hres))
            return hres;

        if(This->ref) {
            list_remove(&This->entry);
            list_add_tail(&ctx->objects, &This->entry);
        }
        script_release(This->ctx);
        script_addref(ctx);
        This->ctx = ctx;

        hres = jsdisp_change_prototype(This, prot);
        if(prot)
            jsdisp_release(prot);
        if(FAILED(hres))
            return hres;
    }

    /* It's safe to repopulate the builtin proxy props now, since the mode is already locked */
    for(prop = This->props, end = prop + This->prop_cnt; prop < end; prop++) {
        struct proxy_prop_info info;

        if(prop->type == PROP_PROXY)
            prop->type = PROP_DELETED;
        else {
            prop->flags |= PROPF_CONFIGURABLE;
            delete_prop(This, prop, &b);
        }

        hres = This->proxy->lpVtbl->PropGetInfo(This->proxy, prop->name, FALSE, &info);
        if(hres == S_OK)
            alloc_proxy_prop(This, &info, &prop);
    }

    /* Populate the constructors on the window */
    hres = get_proxy_default_constructor(ctx, This, &ctor);
    assert(FAILED(hres) || ctor == NULL);
    return hres;
}

static IDispatch* WINAPI WineDispatchProxyCbPrivate_CreateConstructor(IWineDispatchProxyCbPrivate *iface,
        IDispatch *disp, const char *name)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    jsdisp_t *ctor;
    HRESULT hres;

    hres = create_proxy_constructor(disp, name, This, &ctor);
    return SUCCEEDED(hres) ? (IDispatch*)&ctor->IDispatchEx_iface : NULL;
}

static HRESULT WINAPI WineDispatchProxyCbPrivate_DefineConstructor(IWineDispatchProxyCbPrivate *iface,
        const char *name, IDispatch *prot_disp, IDispatch *ctor_disp)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    jsval_t val = jsval_disp(prot_disp);
    jsdisp_t *prot, *ctor;
    unsigned i = 0, hash;
    dispex_prop_t *prop;
    WCHAR nameW[64];
    HRESULT hres;
    BOOL b;

    do nameW[i] = name[i]; while(name[i++]);
    assert(i <= ARRAY_SIZE(nameW));

    hres = convert_to_proxy(This->ctx, &val);
    if(FAILED(hres))
        return hres;
    prot = as_jsdisp(get_object(val));

    if(ctor_disp)
        hres = create_proxy_constructor(ctor_disp, name, prot, &ctor);
    else {
        /* The prototype's proxy should have already set up the constructor, so this can't fail */
        IDispatch *tmp_disp;
        prot->proxy->lpVtbl->GetDefaultConstructor(prot->proxy, This->proxy, &tmp_disp);
        val = jsval_disp(tmp_disp);
        convert_to_proxy(This->ctx, &val);
        ctor = as_jsdisp(get_object(val));
    }
    jsdisp_release(prot);
    if(FAILED(hres))
        return hres;

    /* Remove the builtin proxy prop from the prototype (first time only), since it's part of the object itself */
    hash = string_hash(nameW);
    if(!find_prop_name_raw(This->prototype, hash, nameW, FALSE) && !alloc_prop(This->prototype, nameW, PROP_DELETED, 0)) {
        hres = E_OUTOFMEMORY;
        goto end;
    }

    /* Define the constructor forcefully, so make sure to not look into the underlying proxy dispids,
       otherwise it might pick up elements by this id. And if any found, force it to be configurable. */
    prop = find_prop_name_raw(This, hash, nameW, FALSE);
    if(prop) {
        prop->flags |= PROPF_CONFIGURABLE;
        delete_prop(This, prop, &b);
    }else if(!(prop = alloc_prop(This, nameW, PROP_DELETED, 0))) {
        hres = E_OUTOFMEMORY;
        goto end;
    }

    hres = jsval_copy(jsval_obj(ctor), &prop->u.val);
    if(FAILED(hres))
        goto end;
    prop->type = PROP_JSVAL;
    prop->flags = PROPF_WRITABLE | PROPF_CONFIGURABLE;

end:
    jsdisp_release(ctor);
    return hres;
}

static HRESULT WINAPI WineDispatchProxyCbPrivate_CreateObject(IWineDispatchProxyCbPrivate *iface, IDispatchEx **obj)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    jsdisp_t *jsdisp;
    HRESULT hres;

    hres = create_object(This->ctx, NULL, &jsdisp);
    if(SUCCEEDED(hres))
        *obj = &jsdisp->IDispatchEx_iface;
    return hres;
}

static HRESULT WINAPI WineDispatchProxyCbPrivate_PropEnum(IWineDispatchProxyCbPrivate *iface, const WCHAR *name)
{
    jsdisp_t *This = impl_from_IWineDispatchProxyCbPrivate(iface);
    dispex_prop_t *prop;

    return find_prop_name(This, string_hash(name), name, FALSE, &prop);
}

static IWineDispatchProxyCbPrivateVtbl WineDispatchProxyCbPrivateVtbl = {
    {
    DispatchEx_QueryInterface,
    DispatchEx_AddRef,
    DispatchEx_Release,
    DispatchEx_GetTypeInfoCount,
    DispatchEx_GetTypeInfo,
    DispatchEx_GetIDsOfNames,
    DispatchEx_Invoke,
    DispatchEx_GetDispID,
    DispatchEx_InvokeEx,
    DispatchEx_DeleteMemberByName,
    DispatchEx_DeleteMemberByDispID,
    DispatchEx_GetMemberProperties,
    DispatchEx_GetMemberName,
    DispatchEx_GetNextDispID,
    DispatchEx_GetNameSpaceParent
    },

    /* IWineDispatchProxyCbPrivate extension */
    WineDispatchProxyCbPrivate_InitProxy,
    WineDispatchProxyCbPrivate_Unlinked,
    WineDispatchProxyCbPrivate_HostUpdated,
    WineDispatchProxyCbPrivate_CreateConstructor,
    WineDispatchProxyCbPrivate_DefineConstructor,
    WineDispatchProxyCbPrivate_CreateObject,
    WineDispatchProxyCbPrivate_CreateArrayBuffer,
    WineDispatchProxyCbPrivate_GetRandomValues,
    WineDispatchProxyCbPrivate_PropEnum
};

jsdisp_t *as_jsdisp(IDispatch *disp)
{
    assert(disp->lpVtbl == (IDispatchVtbl*)&WineDispatchProxyCbPrivateVtbl);
    return impl_from_IDispatchEx((IDispatchEx*)disp);
}

jsdisp_t *to_jsdisp(IDispatch *disp)
{
    return disp->lpVtbl == (IDispatchVtbl*)&WineDispatchProxyCbPrivateVtbl ? impl_from_IDispatchEx((IDispatchEx*)disp) : NULL;
}

HRESULT init_dispex(jsdisp_t *dispex, script_ctx_t *ctx, const builtin_info_t *builtin_info, jsdisp_t *prototype)
{
    unsigned i;

    /* FIXME: Use better heuristics to decide when to run the GC */
    if(GetTickCount() - ctx->gc_last_tick > 30000)
        gc_run(ctx, FALSE);

    TRACE("%p (%p)\n", dispex, prototype);

    dispex->IDispatchEx_iface.lpVtbl = (const IDispatchExVtbl*)&WineDispatchProxyCbPrivateVtbl;
    dispex->ref = 1;
    dispex->builtin_info = builtin_info;
    dispex->extensible = TRUE;
    dispex->prop_cnt = 0;

    dispex->props = calloc(1, sizeof(dispex_prop_t)*(dispex->buf_size=4));
    if(!dispex->props)
        return E_OUTOFMEMORY;

    for(i = 0; i < dispex->buf_size; i++) {
        dispex->props[i].bucket_head = ~0;
        dispex->props[i].bucket_next = ~0;
    }

    dispex->prototype = prototype;
    if(prototype)
        jsdisp_addref(prototype);

    script_addref(ctx);
    dispex->ctx = ctx;

    list_add_tail(&ctx->objects, &dispex->entry);
    return S_OK;
}

static const builtin_info_t dispex_info = {
    JSCLASS_NONE,
    NULL,
    0, NULL,
    NULL,
    NULL
};

HRESULT create_dispex(script_ctx_t *ctx, const builtin_info_t *builtin_info, jsdisp_t *prototype, jsdisp_t **dispex)
{
    jsdisp_t *ret;
    HRESULT hres;

    *dispex = NULL;
    ret = calloc(1, sizeof(jsdisp_t));
    if(!ret)
        return E_OUTOFMEMORY;

    hres = init_dispex(ret, ctx, builtin_info ? builtin_info : &dispex_info, prototype);
    if(FAILED(hres)) {
        free(ret);
        return hres;
    }

    *dispex = ret;
    return S_OK;
}

static const builtin_info_t proxy_dispex_info = {
    JSCLASS_OBJECT,
    NULL,
    0, NULL,
    NULL,
    NULL
};

HRESULT convert_to_proxy(script_ctx_t *ctx, jsval_t *val)
{
    IWineDispatchProxyCbPrivate **proxy_ref;
    IWineDispatchProxyPrivate *proxy;
    jsdisp_t *jsdisp, *prot, *ctor;
    IDispatch *obj;
    HRESULT hres;

    if(ctx->version < SCRIPTLANGUAGEVERSION_ES5 || !val || !is_object_instance(*val))
        return S_OK;
    obj = get_object(*val);
    if(to_jsdisp(obj))
        return S_OK;

    if(FAILED(IDispatch_QueryInterface(obj, &IID_IWineDispatchProxyPrivate, (void**)&proxy)) || !proxy)
        return S_OK;
    IDispatch_Release(obj);

    if(!*(proxy_ref = proxy->lpVtbl->GetProxyFieldRef(proxy))) {
        if(!ctx->global) {
            FIXME("Script is uninitialized?\n");
            hres = E_UNEXPECTED;
            goto fail;
        }

        hres = get_proxy_default_prototype(ctx, proxy, &prot);
        if(FAILED(hres))
            goto fail;
        if(!prot)
            return hres;  /* not a JS object */

        /* It's possible for get_proxy_default_prototype to have initialized the proxy ref,
           e.g. locking the document mode while obtaining the dispex info, so re-check it. */
        if(!*proxy_ref) {
            hres = create_dispex(ctx, &proxy_dispex_info, prot, &jsdisp);
            jsdisp_release(prot);
            if(FAILED(hres))
                goto fail;

            *proxy_ref = (IWineDispatchProxyCbPrivate*)&jsdisp->IDispatchEx_iface;
            jsdisp->proxy = proxy;

            hres = get_proxy_default_constructor(ctx, jsdisp, &ctor);
            if(SUCCEEDED(hres) && ctor) {
                hres = jsdisp_define_data_property(jsdisp, L"constructor", PROPF_WRITABLE | PROPF_CONFIGURABLE, jsval_obj(ctor));
                jsdisp_release(ctor);
            }
            if(FAILED(hres)) {
                *proxy_ref = NULL;
                jsdisp->proxy = NULL;
                jsdisp_release(jsdisp);
                goto fail;
            }

            *val = jsval_obj(jsdisp);
            return S_OK;
        }

        jsdisp_release(prot);
    }

    /* Re-acquire the proxy if it's an old dangling proxy */
    jsdisp = impl_from_IWineDispatchProxyCbPrivate(*proxy_ref);
    assert(jsdisp->proxy == proxy);

    if(!jsdisp->ref++)
        list_add_tail(&jsdisp->ctx->objects, &jsdisp->entry);
    else
        IDispatchEx_Release((IDispatchEx*)proxy);  /* already held by jsdisp */

    TRACE("re-acquired %p\n", jsdisp);
    *val = jsval_obj(jsdisp);
    return S_OK;

fail:
    IDispatchEx_Release((IDispatchEx*)proxy);
    return hres;
}

void jsdisp_free(jsdisp_t *obj)
{
    dispex_prop_t *prop;

    list_remove(&obj->entry);

    /* If it's a proxy, stay alive and keep it associated with the disp, since
       we can be re-acquired at some later point. When the underlying disp is
       actually destroyed, it should unlink us and then we free it for real. */
    if(obj->proxy) {
        list_init(&obj->entry);
        IDispatchEx_Release((IDispatchEx*)obj->proxy);
        return;
    }

    TRACE("(%p)\n", obj);

    if(obj->has_weak_refs) {
        struct list *list = &RB_ENTRY_VALUE(rb_get(&obj->ctx->weak_refs, obj), struct weak_refs_entry, entry)->list;
        do {
            remove_weakmap_entry(LIST_ENTRY(list->next, struct weakmap_entry, weak_refs_entry));
        } while(obj->has_weak_refs);
    }

    for(prop = obj->props; prop < obj->props+obj->prop_cnt; prop++) {
        switch(prop->type) {
        case PROP_JSVAL:
            jsval_release(prop->u.val);
            break;
        case PROP_ACCESSOR:
            if(prop->u.accessor.getter)
                jsdisp_release(prop->u.accessor.getter);
            if(prop->u.accessor.setter)
                jsdisp_release(prop->u.accessor.setter);
            break;
        default:
            break;
        };
        free(prop->name);
    }
    free(obj->props);
    script_release(obj->ctx);
    if(obj->prototype)
        jsdisp_release(obj->prototype);

    if(obj->builtin_info->destructor)
        obj->builtin_info->destructor(obj);
    else
        free(obj);
}

void jsdisp_reacquire(jsdisp_t *jsdisp)
{
    list_add_tail(&jsdisp->ctx->objects, &jsdisp->entry);
    if(jsdisp->proxy)
        IDispatchEx_AddRef((IDispatchEx*)jsdisp->proxy);
}

#ifdef TRACE_REFCNT

jsdisp_t *jsdisp_addref(jsdisp_t *jsdisp)
{
    ULONG ref = ++jsdisp->ref;
    TRACE("(%p) ref=%ld\n", jsdisp, ref);
    if(ref == 1)
        jsdisp_reacquire(jsdisp);
    return jsdisp;
}

void jsdisp_release(jsdisp_t *jsdisp)
{
    ULONG ref = --jsdisp->ref;

    TRACE("(%p) ref=%ld\n", jsdisp, ref);

    if(!ref)
        jsdisp_free(jsdisp);
}

#endif

HRESULT init_dispex_from_constr(jsdisp_t *dispex, script_ctx_t *ctx, const builtin_info_t *builtin_info, jsdisp_t *constr)
{
    jsdisp_t *prot = NULL;
    dispex_prop_t *prop;
    HRESULT hres;

    hres = find_prop_name_prot(constr, string_hash(L"prototype"), L"prototype", FALSE, &prop);
    if(SUCCEEDED(hres) && prop && prop->type!=PROP_DELETED) {
        jsval_t val;

        hres = prop_get(constr, to_disp(constr), prop, &val, &ctx->jscaller->IServiceProvider_iface);
        if(FAILED(hres)) {
            ERR("Could not get prototype\n");
            return hres;
        }

        if(is_object_instance(val))
            prot = iface_to_jsdisp(get_object(val));
        else
            prot = jsdisp_addref(ctx->object_prototype);

        jsval_release(val);
    }

    hres = init_dispex(dispex, ctx, builtin_info, prot);

    if(prot)
        jsdisp_release(prot);
    return hres;
}

jsdisp_t *iface_to_jsdisp(IDispatch *iface)
{
    return iface->lpVtbl == (const IDispatchVtbl*)&WineDispatchProxyCbPrivateVtbl
        ? jsdisp_addref( impl_from_IDispatchEx((IDispatchEx*)iface))
        : NULL;
}

static HRESULT WINAPI jsdisp_cc_traverse(void *ccp, void *p, nsCycleCollectionTraversalCallback *cb)
{
    jsdisp_t *This = impl_from_IDispatchEx(p);
    note_edge_t note_edge = cc_api.note_edge;
    dispex_prop_t *prop = This->props, *end;

    /* If we have a proxy, we don't actually let it hold ref to us (to prevent cyclic refs on every one of them),
       but we remain alive even with 0 refcount until it notifies us of being unlinked. However, for the CC to
       work properly, we need to fake our reported refcount to it by letting it assume it does hold a ref to us. */
    cc_api.describe_node(This->ref + (This->proxy != NULL), "jsdisp", cb);

    for(end = prop + This->prop_cnt; prop < end; prop++) {
        switch(prop->type) {
        case PROP_JSVAL:
            if(is_object_instance(prop->u.val))
                note_edge((nsISupports*)get_object(prop->u.val), "prop", cb);
            break;
        case PROP_ACCESSOR:
            if(prop->u.accessor.getter)
                note_edge((nsISupports*)&prop->u.accessor.getter->IDispatchEx_iface, "prop", cb);
            if(prop->u.accessor.setter)
                note_edge((nsISupports*)&prop->u.accessor.setter->IDispatchEx_iface, "prop", cb);
            break;
        default:
            break;
        }
    }

    if(This->prototype)
        note_edge((nsISupports*)&This->prototype->IDispatchEx_iface, "prototype", cb);

    if(This->builtin_info->cc_traverse)
        This->builtin_info->cc_traverse(This, cb);

    /* We hold a ref when we're not kept alive only by the proxy */
    if(This->proxy && This->ref)
        note_edge((nsISupports*)This->proxy, "proxy", cb);

    return S_OK;
}

static HRESULT WINAPI jsdisp_cc_unlink(void *p)
{
    jsdisp_t *This = impl_from_IDispatchEx(p);

    unlink_jsdisp(This);
    return S_OK;
}

static BOOL __cdecl cc_api_stub_is_full_cc(void) { return FALSE; }
static void __cdecl cc_api_stub_collect(IActiveScriptSite *site, BOOL force) { }

struct proxy_cc_api cc_api = {
    .is_full_cc    = cc_api_stub_is_full_cc,
    .collect       = cc_api_stub_collect,
};

void init_cc_api(IDispatch *disp)
{
    static const CCObjCallback jsdisp_ccp_callback = {
        jsdisp_cc_traverse,
        jsdisp_cc_unlink,
        NULL  /* delete_cycle_collectable shouldn't ever be called, since we're never part of the purple buffer */
    };
    IWineDispatchProxyPrivate *proxy;

    if(SUCCEEDED(IDispatch_QueryInterface(disp, &IID_IWineDispatchProxyPrivate, (void**)&proxy)) && proxy) {
        proxy->lpVtbl->InitCC(&cc_api, &jsdisp_ccp_callback);
        IDispatchEx_Release((IDispatchEx*)proxy);
    }
}

HRESULT jsdisp_get_id(jsdisp_t *jsdisp, const WCHAR *name, DWORD flags, DISPID *id)
{
    dispex_prop_t *prop;
    unsigned idx;
    HRESULT hres;

    if(override_idx(jsdisp, name, &idx)) {
        if(idx >= jsdisp->builtin_info->idx_length(jsdisp)) {
            *id = DISPID_UNKNOWN;
            return DISP_E_UNKNOWNNAME;
        }
        hres = find_prop_name(jsdisp, string_hash(name), name, FALSE, &prop);
    }
    else if(jsdisp->extensible && (flags & fdexNameEnsure))
        hres = ensure_prop_name(jsdisp, name, PROPF_ENUMERABLE | PROPF_CONFIGURABLE | PROPF_WRITABLE,
                                flags & fdexNameCaseInsensitive, &prop);
    else
        hres = find_prop_name_prot(jsdisp, string_hash(name), name, flags & fdexNameCaseInsensitive, &prop);
    if(FAILED(hres))
        return hres;

    if(prop && prop->type!=PROP_DELETED) {
        *id = prop_to_id(jsdisp, prop);
        return S_OK;
    }

    TRACE("not found %s\n", debugstr_w(name));
    *id = DISPID_UNKNOWN;
    return DISP_E_UNKNOWNNAME;
}

HRESULT jsdisp_get_idx_id(jsdisp_t *jsdisp, DWORD idx, DISPID *id)
{
    WCHAR name[11];

    swprintf(name, ARRAY_SIZE(name), L"%u", idx);
    return jsdisp_get_id(jsdisp, name, 0, id);
}

HRESULT jsdisp_call_value(jsdisp_t *jsfunc, jsval_t vthis, WORD flags, unsigned argc, jsval_t *argv,
        jsval_t *r, IServiceProvider *caller)
{
    HRESULT hres;

    assert(!(flags & ~(DISPATCH_METHOD|DISPATCH_CONSTRUCT|DISPATCH_JSCRIPT_INTERNAL_MASK)));

    if(is_class(jsfunc, JSCLASS_FUNCTION)) {
        hres = Function_invoke(jsfunc, vthis, flags, argc, argv, r, caller);
    }else if(jsfunc->proxy) {
        hres = proxy_disp_call(jsfunc, vthis, DISPID_VALUE, flags, argc, argv, r, caller);
    }else {
        if(!jsfunc->builtin_info->call) {
            WARN("Not a function\n");
            return JS_E_FUNCTION_EXPECTED;
        }

        if(jsfunc->ctx->state == SCRIPTSTATE_UNINITIALIZED || jsfunc->ctx->state == SCRIPTSTATE_CLOSED)
            return E_UNEXPECTED;

        flags &= ~DISPATCH_JSCRIPT_INTERNAL_MASK;
        hres = jsfunc->builtin_info->call(jsfunc->ctx, vthis, flags, argc, argv, r);
    }
    if(SUCCEEDED(hres))
        hres = convert_to_proxy(jsfunc->ctx, r);
    return hres;
}

HRESULT jsdisp_call(jsdisp_t *disp, DISPID id, WORD flags, unsigned argc, jsval_t *argv, jsval_t *r)
{
    dispex_prop_t *prop;

    prop = get_prop(disp, id);
    if(!prop)
        return DISP_E_MEMBERNOTFOUND;

    return invoke_prop_func(disp, to_disp(disp), prop, flags, argc, argv, r, &disp->ctx->jscaller->IServiceProvider_iface);
}

HRESULT jsdisp_call_name(jsdisp_t *disp, const WCHAR *name, WORD flags, unsigned argc, jsval_t *argv, jsval_t *r)
{
    dispex_prop_t *prop;
    HRESULT hres;

    hres = find_prop_name_prot(disp, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;

    if(!prop || prop->type == PROP_DELETED)
        return JS_E_INVALID_PROPERTY;

    hres = invoke_prop_func(disp, to_disp(disp), prop, flags, argc, argv, r, &disp->ctx->jscaller->IServiceProvider_iface);
    return (hres == DISP_E_MEMBERNOTFOUND) ? JS_E_INVALID_PROPERTY : hres;
}

static HRESULT disp_invoke(script_ctx_t *ctx, IDispatch *disp, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *r,
        IServiceProvider *caller)
{
    IDispatchEx *dispex;
    EXCEPINFO ei;
    HRESULT hres;

    memset(&ei, 0, sizeof(ei));
    hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    if(SUCCEEDED(hres)) {
        hres = IDispatchEx_InvokeEx(dispex, id, ctx->lcid, flags, params, r, &ei, caller);
        IDispatchEx_Release(dispex);
    }else {
        UINT err = 0;

        if(flags == DISPATCH_CONSTRUCT) {
            WARN("IDispatch cannot be constructor\n");
            return DISP_E_MEMBERNOTFOUND;
        }

        if(params->cNamedArgs == 1 && params->rgdispidNamedArgs[0] == DISPID_THIS) {
            params->cNamedArgs = 0;
            params->rgdispidNamedArgs = NULL;
            params->cArgs--;
            params->rgvarg++;
        }

        TRACE("using IDispatch\n");
        hres = IDispatch_Invoke(disp, id, &IID_NULL, ctx->lcid, flags, params, r, &ei, &err);
    }

    if(hres == DISP_E_EXCEPTION)
        disp_fill_exception(ctx, &ei);

    return hres;
}

void disp_fill_exception(script_ctx_t *ctx, EXCEPINFO *ei)
{
    TRACE("DISP_E_EXCEPTION: %08lx %s %s\n", ei->scode, debugstr_w(ei->bstrSource), debugstr_w(ei->bstrDescription));
    reset_ei(ctx->ei);
    if(ei->pfnDeferredFillIn)
        ei->pfnDeferredFillIn(ei);
    ctx->ei->error = (SUCCEEDED(ei->scode) || ei->scode == DISP_E_EXCEPTION) ? E_FAIL : ei->scode;
    if(ei->bstrSource)
        ctx->ei->source = jsstr_alloc_len(ei->bstrSource, SysStringLen(ei->bstrSource));
    if(ei->bstrDescription)
        ctx->ei->message = jsstr_alloc_len(ei->bstrDescription, SysStringLen(ei->bstrDescription));
    SysFreeString(ei->bstrSource);
    SysFreeString(ei->bstrDescription);
    SysFreeString(ei->bstrHelpFile);
}

HRESULT disp_call(script_ctx_t *ctx, IDispatch *disp, DISPID id, WORD flags, unsigned argc, jsval_t *argv, jsval_t *ret)
{
    VARIANT buf[6], retv;
    jsdisp_t *jsdisp;
    DISPPARAMS dp;
    unsigned i;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp && jsdisp->ctx == ctx) {
        if(flags & DISPATCH_PROPERTYPUT) {
            FIXME("disp_call(propput) on builtin object\n");
            jsdisp_release(jsdisp);
            return E_FAIL;
        }

        if(ctx != jsdisp->ctx)
            flags &= ~DISPATCH_JSCRIPT_INTERNAL_MASK;
        hres = jsdisp_call(jsdisp, id, flags, argc, argv, ret);
        jsdisp_release(jsdisp);
        return hres;
    }
    if(jsdisp)
        jsdisp_release(jsdisp);

    flags &= ~DISPATCH_JSCRIPT_INTERNAL_MASK;
    if(ret && argc)
        flags |= DISPATCH_PROPERTYGET;

    dp.cArgs = argc;

    if(flags & DISPATCH_PROPERTYPUT) {
        static DISPID propput_dispid = DISPID_PROPERTYPUT;

        dp.cNamedArgs = 1;
        dp.rgdispidNamedArgs = &propput_dispid;
    }else {
        dp.cNamedArgs = 0;
        dp.rgdispidNamedArgs = NULL;
    }

    if(dp.cArgs > ARRAY_SIZE(buf)) {
        dp.rgvarg = malloc(argc * sizeof(VARIANT));
        if(!dp.rgvarg)
            return E_OUTOFMEMORY;
    }else {
        dp.rgvarg = buf;
    }

    for(i=0; i<argc; i++) {
        hres = jsval_to_variant(argv[i], dp.rgvarg+argc-i-1);
        if(FAILED(hres)) {
            while(i--)
                VariantClear(dp.rgvarg+argc-i-1);
            if(dp.rgvarg != buf)
                free(dp.rgvarg);
            return hres;
        }
    }

    V_VT(&retv) = VT_EMPTY;
    hres = disp_invoke(ctx, disp, id, flags, &dp, ret ? &retv : NULL, &ctx->jscaller->IServiceProvider_iface);

    for(i=0; i<argc; i++)
        VariantClear(dp.rgvarg+argc-i-1);
    if(dp.rgvarg != buf)
        free(dp.rgvarg);

    if(SUCCEEDED(hres) && ret)
        hres = variant_to_jsval(ctx, &retv, ret);
    VariantClear(&retv);
    return hres;
}

HRESULT disp_call_name(script_ctx_t *ctx, IDispatch *disp, const WCHAR *name, WORD flags, unsigned argc, jsval_t *argv, jsval_t *ret)
{
    IDispatchEx *dispex;
    jsdisp_t *jsdisp;
    HRESULT hres;
    DISPID id;
    BSTR bstr;

    if((jsdisp = to_jsdisp(disp)) && jsdisp->ctx == ctx)
        return jsdisp_call_name(jsdisp, name, flags, argc, argv, ret);

    if(!(bstr = SysAllocString(name)))
        return E_OUTOFMEMORY;
    hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    if(SUCCEEDED(hres) && dispex) {
        hres = IDispatchEx_GetDispID(dispex, bstr, make_grfdex(ctx, fdexNameCaseSensitive), &id);
        IDispatchEx_Release(dispex);
    }else {
        hres = IDispatch_GetIDsOfNames(disp, &IID_NULL, &bstr, 1, 0, &id);
    }
    SysFreeString(bstr);
    if(FAILED(hres))
        return hres;

    return disp_call(ctx, disp, id, flags, argc, argv, ret);
}

HRESULT disp_call_value_with_caller(script_ctx_t *ctx, IDispatch *disp, jsval_t vthis, WORD flags, unsigned argc,
        jsval_t *argv, jsval_t *r, IServiceProvider *caller)
{
    VARIANT buf[6], retv, *args = buf;
    IDispatch *jsthis;
    jsdisp_t *jsdisp;
    DISPPARAMS dp;
    unsigned i;
    HRESULT hres = S_OK;

    static DISPID this_id = DISPID_THIS;

    assert(!(flags & ~(DISPATCH_METHOD|DISPATCH_CONSTRUCT|DISPATCH_JSCRIPT_INTERNAL_MASK)));

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp && jsdisp->ctx == ctx) {
        hres = jsdisp_call_value(jsdisp, vthis, flags, argc, argv, r, caller);
        jsdisp_release(jsdisp);
        return hres;
    }
    if(jsdisp)
        jsdisp_release(jsdisp);

    if(is_object_instance(vthis) && (ctx->version < SCRIPTLANGUAGEVERSION_ES5 ||
       ((jsdisp = to_jsdisp(get_object(vthis))) && is_class(jsdisp, JSCLASS_OBJECT) && !jsdisp->proxy)))
        jsthis = get_object(vthis);
    else
        jsthis = NULL;

    flags &= ~DISPATCH_JSCRIPT_INTERNAL_MASK;
    if(r && argc && flags == DISPATCH_METHOD)
        flags |= DISPATCH_PROPERTYGET;

    if(jsthis) {
        dp.cArgs = argc + 1;
        dp.cNamedArgs = 1;
        dp.rgdispidNamedArgs = &this_id;
    }else {
        dp.cArgs = argc;
        dp.cNamedArgs = 0;
        dp.rgdispidNamedArgs = NULL;
    }

    if(dp.cArgs > ARRAY_SIZE(buf) && !(args = malloc(dp.cArgs * sizeof(VARIANT))))
        return E_OUTOFMEMORY;
    dp.rgvarg = args;

    if(jsthis) {
        V_VT(dp.rgvarg) = VT_DISPATCH;
        V_DISPATCH(dp.rgvarg) = jsthis;
    }

    for(i=0; SUCCEEDED(hres) && i < argc; i++)
        hres = jsval_to_variant(argv[i], dp.rgvarg+dp.cArgs-i-1);

    if(SUCCEEDED(hres)) {
        V_VT(&retv) = VT_EMPTY;
        hres = disp_invoke(ctx, disp, DISPID_VALUE, flags, &dp, r ? &retv : NULL, caller);
    }

    for(i = 0; i < argc; i++)
        VariantClear(dp.rgvarg + dp.cArgs - i - 1);
    if(args != buf)
        free(args);

    if(FAILED(hres))
        return hres;
    if(!r)
        return S_OK;

    hres = variant_to_jsval(ctx, &retv, r);
    VariantClear(&retv);
    return hres;
}

HRESULT jsdisp_propput(jsdisp_t *obj, const WCHAR *name, DWORD flags, BOOL throw, jsval_t val)
{
    dispex_prop_t *prop;
    unsigned idx;
    HRESULT hres;

    if(override_idx(obj, name, &idx))
        return obj->builtin_info->idx_put(obj, idx, val);

    if(obj->extensible)
        hres = ensure_prop_name(obj, name, flags, FALSE, &prop);
    else
        hres = find_prop_name(obj, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;
    if(!prop || (prop->type == PROP_DELETED && !obj->extensible))
        return throw ? JS_E_INVALID_ACTION : S_OK;

    return prop_put(obj, prop, val, &obj->ctx->jscaller->IServiceProvider_iface);
}

HRESULT jsdisp_propput_name(jsdisp_t *obj, const WCHAR *name, jsval_t val)
{
    return jsdisp_propput(obj, name, PROPF_ENUMERABLE | PROPF_CONFIGURABLE | PROPF_WRITABLE, FALSE, val);
}

HRESULT jsdisp_propput_idx(jsdisp_t *obj, DWORD idx, jsval_t val)
{
    WCHAR buf[12];

    swprintf(buf, ARRAY_SIZE(buf), L"%d", idx);
    return jsdisp_propput(obj, buf, PROPF_ENUMERABLE | PROPF_CONFIGURABLE | PROPF_WRITABLE, TRUE, val);
}

HRESULT disp_propput(script_ctx_t *ctx, IDispatch *disp, DISPID id, jsval_t val)
{
    jsdisp_t *jsdisp;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp && jsdisp->ctx == ctx) {
        dispex_prop_t *prop;

        prop = get_prop(jsdisp, id);
        if(prop)
            hres = prop_put(jsdisp, prop, val, &ctx->jscaller->IServiceProvider_iface);
        else
            hres = DISP_E_MEMBERNOTFOUND;

        jsdisp_release(jsdisp);
    }else {
        DISPID dispid = DISPID_PROPERTYPUT;
        DWORD flags = DISPATCH_PROPERTYPUT;
        VARIANT var;
        DISPPARAMS dp  = {&var, &dispid, 1, 1};

        if(jsdisp)
            jsdisp_release(jsdisp);
        hres = jsval_to_variant(val, &var);
        if(FAILED(hres))
            return hres;

        if(V_VT(&var) == VT_DISPATCH)
            flags |= DISPATCH_PROPERTYPUTREF;

        hres = disp_invoke(ctx, disp, id, flags, &dp, NULL, &ctx->jscaller->IServiceProvider_iface);
        VariantClear(&var);
    }

    return hres;
}

HRESULT disp_propput_name(script_ctx_t *ctx, IDispatch *disp, const WCHAR *name, jsval_t val)
{
    jsdisp_t *jsdisp;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(!jsdisp || jsdisp->ctx != ctx) {
        IDispatchEx *dispex;
        BSTR str;
        DISPID id;

        if(jsdisp)
            jsdisp_release(jsdisp);
        if(!(str = SysAllocString(name)))
            return E_OUTOFMEMORY;

        hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
        if(SUCCEEDED(hres)) {
            hres = IDispatchEx_GetDispID(dispex, str, make_grfdex(ctx, fdexNameEnsure|fdexNameCaseSensitive), &id);
            IDispatchEx_Release(dispex);
        }else {
            TRACE("using IDispatch\n");
            hres = IDispatch_GetIDsOfNames(disp, &IID_NULL, &str, 1, 0, &id);
        }
        SysFreeString(str);
        if(FAILED(hres))
            return hres;

        return disp_propput(ctx, disp, id, val);
    }

    hres = jsdisp_propput_name(jsdisp, name, val);
    jsdisp_release(jsdisp);
    return hres;
}

HRESULT jsdisp_propget_name(jsdisp_t *obj, const WCHAR *name, jsval_t *val)
{
    dispex_prop_t *prop;
    unsigned idx;
    HRESULT hres;

    if(override_idx(obj, name, &idx))
        return obj->builtin_info->idx_get(obj, idx, val);

    hres = find_prop_name_prot(obj, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;

    if(!prop || prop->type==PROP_DELETED) {
        *val = jsval_undefined();
        return S_OK;
    }

    hres = prop_get(obj, to_disp(obj), prop, val, &obj->ctx->jscaller->IServiceProvider_iface);
    if(hres == DISP_E_MEMBERNOTFOUND) {
        *val = jsval_undefined();
        return S_OK;
    }
    return hres;
}

HRESULT jsdisp_get_idx(jsdisp_t *obj, DWORD idx, jsval_t *r)
{
    WCHAR name[12];
    dispex_prop_t *prop;
    HRESULT hres;

    if(obj->builtin_info->class >= FIRST_TYPEDARRAY_JSCLASS && obj->builtin_info->class <= LAST_TYPEDARRAY_JSCLASS)
        return obj->builtin_info->idx_get(obj, idx, r);

    swprintf(name, ARRAY_SIZE(name), L"%d", idx);

    hres = find_prop_name_prot(obj, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;

    if(!prop || prop->type==PROP_DELETED) {
        *r = jsval_undefined();
        return DISP_E_UNKNOWNNAME;
    }

    hres = prop_get(obj, to_disp(obj), prop, r, &obj->ctx->jscaller->IServiceProvider_iface);
    if(hres == DISP_E_MEMBERNOTFOUND) {
        *r = jsval_undefined();
        return DISP_E_UNKNOWNNAME;
    }
    return hres;
}

HRESULT jsdisp_propget(jsdisp_t *jsdisp, DISPID id, jsval_t *val)
{
    dispex_prop_t *prop;

    prop = get_prop(jsdisp, id);
    if(!prop)
        return DISP_E_MEMBERNOTFOUND;

    return prop_get(jsdisp, to_disp(jsdisp), prop, val, &jsdisp->ctx->jscaller->IServiceProvider_iface);
}

HRESULT disp_propget(script_ctx_t *ctx, IDispatch *disp, DISPID id, jsval_t *val)
{
    DISPPARAMS dp  = {NULL,NULL,0,0};
    jsdisp_t *jsdisp;
    VARIANT var;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp && jsdisp->ctx == ctx) {
        hres = jsdisp_propget(jsdisp, id, val);
        jsdisp_release(jsdisp);
        return hres;
    }
    if(jsdisp)
        jsdisp_release(jsdisp);

    V_VT(&var) = VT_EMPTY;
    hres = disp_invoke(ctx, disp, id, INVOKE_PROPERTYGET, &dp, &var, &ctx->jscaller->IServiceProvider_iface);
    if(SUCCEEDED(hres)) {
        hres = variant_to_jsval(ctx, &var, val);
        VariantClear(&var);
    }
    return hres;
}

HRESULT jsdisp_delete_idx(jsdisp_t *obj, DWORD idx)
{
    WCHAR buf[12];
    dispex_prop_t *prop;
    BOOL b;
    HRESULT hres;

    if(obj->builtin_info->class >= FIRST_TYPEDARRAY_JSCLASS && obj->builtin_info->class <= LAST_TYPEDARRAY_JSCLASS)
        return S_OK;

    swprintf(buf, ARRAY_SIZE(buf), L"%d", idx);

    hres = find_prop_name(obj, string_hash(buf), buf, FALSE, &prop);
    if(FAILED(hres) || !prop)
        return hres;

    hres = delete_prop(obj, prop, &b);
    if(FAILED(hres))
        return hres;
    return b ? S_OK : JS_E_INVALID_ACTION;
}

HRESULT disp_delete(IDispatch *disp, DISPID id, BOOL *ret)
{
    IDispatchEx *dispex;
    jsdisp_t *jsdisp;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp) {
        dispex_prop_t *prop;

        prop = get_prop(jsdisp, id);
        if(prop)
            hres = delete_prop(jsdisp, prop, ret);
        else
            hres = DISP_E_MEMBERNOTFOUND;

        jsdisp_release(jsdisp);
        return hres;
    }

    hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    if(FAILED(hres)) {
        *ret = FALSE;
        return S_OK;
    }

    hres = IDispatchEx_DeleteMemberByDispID(dispex, id);
    IDispatchEx_Release(dispex);
    if(FAILED(hres))
        return hres;

    *ret = hres == S_OK;
    return S_OK;
}

HRESULT jsdisp_next_prop(jsdisp_t *obj, DISPID id, enum jsdisp_enum_type enum_type, DISPID *ret)
{
    dispex_prop_t *iter;
    DWORD idx = id;
    HRESULT hres;

    if(id == DISPID_STARTENUM || idx >= obj->prop_cnt) {
        hres = (enum_type == JSDISP_ENUM_ALL) ? fill_protrefs(obj) : fill_props(obj);
        if(FAILED(hres))
            return hres;
        if(id == DISPID_STARTENUM)
            idx = 0;
        if(idx >= obj->prop_cnt)
            return S_FALSE;
    }

    for(iter = &obj->props[idx]; iter < obj->props + obj->prop_cnt; iter++) {
        hres = fix_overridden_prop(obj, iter);
        if(FAILED(hres))
            return hres;
        if(iter->type == PROP_DELETED)
            continue;
        if(enum_type != JSDISP_ENUM_ALL && iter->type == PROP_PROTREF)
            continue;
        if(enum_type != JSDISP_ENUM_OWN && !(get_flags(obj, iter) & PROPF_ENUMERABLE))
            continue;
        *ret = prop_to_id(obj, iter);
        return S_OK;
    }

    if(obj->ctx->html_mode)
        return jsdisp_next_prop(obj, prop_to_id(obj, iter - 1), enum_type, ret);

    return S_FALSE;
}

HRESULT disp_delete_name(script_ctx_t *ctx, IDispatch *disp, jsstr_t *name, BOOL *ret)
{
    IDispatchEx *dispex;
    jsdisp_t *jsdisp;
    BSTR bstr;
    HRESULT hres;

    jsdisp = iface_to_jsdisp(disp);
    if(jsdisp) {
        dispex_prop_t *prop;
        const WCHAR *ptr;
        unsigned idx;

        ptr = jsstr_flatten(name);
        if(!ptr) {
            jsdisp_release(jsdisp);
            return E_OUTOFMEMORY;
        }

        if(override_idx(jsdisp, ptr, &idx)) {
            *ret = FALSE;
            hres = S_OK;
        }else {
            hres = find_prop_name(jsdisp, string_hash(ptr), ptr, FALSE, &prop);
            if(prop) {
                hres = delete_prop(jsdisp, prop, ret);
            }else {
                *ret = TRUE;
                hres = S_OK;
            }
        }

        jsdisp_release(jsdisp);
        return hres;
    }

    bstr = SysAllocStringLen(NULL, jsstr_length(name));
    if(!bstr)
        return E_OUTOFMEMORY;
    jsstr_flush(name, bstr);

    hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    if(SUCCEEDED(hres)) {
        hres = IDispatchEx_DeleteMemberByName(dispex, bstr, make_grfdex(ctx, fdexNameCaseSensitive));
        if(SUCCEEDED(hres))
            *ret = hres == S_OK;
        IDispatchEx_Release(dispex);
    }else {
        DISPID id;

        hres = IDispatch_GetIDsOfNames(disp, &IID_NULL, &bstr, 1, 0, &id);
        if(SUCCEEDED(hres)) {
            /* Property exists and we can't delete it from pure IDispatch interface, so return false. */
            *ret = FALSE;
        }else if(hres == DISP_E_UNKNOWNNAME) {
            /* Property doesn't exist, so nothing to delete */
            *ret = TRUE;
            hres = S_OK;
        }
    }

    SysFreeString(bstr);
    return hres;
}

HRESULT jsdisp_get_own_property(jsdisp_t *obj, const WCHAR *name, BOOL flags_only,
                                property_desc_t *desc)
{
    dispex_prop_t *prop;
    unsigned idx;
    HRESULT hres;

    if(override_idx(obj, name, &idx)) {
        if(idx >= obj->builtin_info->idx_length(obj))
            return DISP_E_UNKNOWNNAME;

        memset(desc, 0, sizeof(*desc));
        if(!flags_only) {
            hres = obj->builtin_info->idx_get(obj, idx, &desc->value);
            if(FAILED(hres))
                return hres;
        }
        desc->flags = PROPF_ENUMERABLE | PROPF_WRITABLE;
        desc->mask  = PROPF_ENUMERABLE | PROPF_WRITABLE | PROPF_CONFIGURABLE;
        desc->explicit_value = TRUE;
        return S_OK;
    }

    hres = find_prop_name(obj, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;

    if(!prop)
        return DISP_E_UNKNOWNNAME;

    memset(desc, 0, sizeof(*desc));

    switch(prop->type) {
    case PROP_BUILTIN:
    case PROP_JSVAL:
    case PROP_IDX:
    case PROP_PROXY:
        desc->mask |= PROPF_WRITABLE;
        desc->explicit_value = TRUE;
        if(!flags_only) {
            hres = prop_get(obj, to_disp(obj), prop, &desc->value, &obj->ctx->jscaller->IServiceProvider_iface);
            if(FAILED(hres))
                return (hres == DISP_E_MEMBERNOTFOUND) ? DISP_E_UNKNOWNNAME : hres;
        }
        break;
    case PROP_ACCESSOR:
        desc->explicit_getter = desc->explicit_setter = TRUE;
        if(!flags_only) {
            desc->getter = prop->u.accessor.getter
                ? jsdisp_addref(prop->u.accessor.getter) : NULL;
            desc->setter = prop->u.accessor.setter
                ? jsdisp_addref(prop->u.accessor.setter) : NULL;
        }
        break;
    default:
        return DISP_E_UNKNOWNNAME;
    }

    desc->flags = prop->flags & (PROPF_ENUMERABLE | PROPF_WRITABLE | PROPF_CONFIGURABLE);
    desc->mask |= PROPF_ENUMERABLE | PROPF_CONFIGURABLE;
    return S_OK;
}

HRESULT jsdisp_define_property(jsdisp_t *obj, const WCHAR *name, property_desc_t *desc)
{
    dispex_prop_t *prop;
    unsigned idx;
    HRESULT hres;

    if(override_idx(obj, name, &idx)) {
        if((desc->flags & desc->mask) != (desc->mask & (PROPF_WRITABLE | PROPF_ENUMERABLE)))
            return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
        if(desc->explicit_value)
            return obj->builtin_info->idx_put(obj, idx, desc->value);
        if(desc->explicit_getter || desc->explicit_setter)
            return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
        return obj->builtin_info->idx_put(obj, idx, jsval_undefined());
    }

    hres = find_prop_name(obj, string_hash(name), name, FALSE, &prop);
    if(FAILED(hres))
        return hres;

    if((!prop || prop->type == PROP_DELETED || prop->type == PROP_PROTREF) && !obj->extensible)
        return throw_error(obj->ctx, JS_E_OBJECT_NONEXTENSIBLE, name);

    if(!prop && !(prop = alloc_prop(obj, name, PROP_DELETED, 0)))
       return E_OUTOFMEMORY;

    if(obj->proxy && desc->explicit_value) {
        struct proxy_prop_info info;

        info.name = name;
        hres = obj->proxy->lpVtbl->PropDefineOverride(obj->proxy, &info);
        if(hres != S_FALSE) {
            dispex_prop_t bak = *prop;
            if(FAILED(hres))
                return hres;
            hres = alloc_proxy_prop(obj, &info, &prop);
            if(SUCCEEDED(hres)) {
                hres = prop_put(obj, prop, desc->value, &obj->ctx->jscaller->IServiceProvider_iface);
                if(SUCCEEDED(hres)) {
                    switch(bak.type) {
                    case PROP_JSVAL:
                        jsval_release(bak.u.val);
                        break;
                    case PROP_ACCESSOR:
                        if(bak.u.accessor.getter) jsdisp_release(bak.u.accessor.getter);
                        if(bak.u.accessor.setter) jsdisp_release(bak.u.accessor.setter);
                        break;
                    default:
                        break;
                    }
                    return S_OK;
                }
            }
            *prop = bak;
            return hres;
        }
    }

    if(prop->type == PROP_DELETED || prop->type == PROP_PROTREF) {
        prop->flags = desc->flags;
        if(desc->explicit_getter || desc->explicit_setter) {
            prop->type = PROP_ACCESSOR;
            prop->u.accessor.getter = desc->getter ? jsdisp_addref(desc->getter) : NULL;
            prop->u.accessor.setter = desc->setter ? jsdisp_addref(desc->setter) : NULL;
            TRACE("%s = accessor { get: %p set: %p }\n", debugstr_w(name),
                  prop->u.accessor.getter, prop->u.accessor.setter);
        }else {
            prop->type = PROP_JSVAL;
            if(desc->explicit_value) {
                hres = jsval_copy(desc->value, &prop->u.val);
                if(FAILED(hres))
                    return hres;
            }else {
                prop->u.val = jsval_undefined();
            }
            TRACE("%s = %s\n", debugstr_w(name), debugstr_jsval(prop->u.val));
        }
        return S_OK;
    }

    TRACE("existing prop %s prop flags %lx desc flags %x desc mask %x\n", debugstr_w(name),
          prop->flags, desc->flags, desc->mask);

    if(!(prop->flags & PROPF_CONFIGURABLE)) {
        if(((desc->mask & PROPF_CONFIGURABLE) && (desc->flags & PROPF_CONFIGURABLE))
           || ((desc->mask & PROPF_ENUMERABLE)
               && ((desc->flags & PROPF_ENUMERABLE) != (prop->flags & PROPF_ENUMERABLE))))
            return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
    }

    if(desc->explicit_value || (desc->mask & PROPF_WRITABLE)) {
        if(prop->type == PROP_ACCESSOR) {
            if(!(prop->flags & PROPF_CONFIGURABLE))
                return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
            if(prop->u.accessor.getter)
                jsdisp_release(prop->u.accessor.getter);
            if(prop->u.accessor.setter)
                jsdisp_release(prop->u.accessor.setter);

            prop->type = PROP_JSVAL;
            hres = jsval_copy(desc->value, &prop->u.val);
            if(FAILED(hres)) {
                prop->u.val = jsval_undefined();
                return hres;
            }
        }else {
            if(!(prop->flags & PROPF_CONFIGURABLE) && !(prop->flags & PROPF_WRITABLE)) {
                if((desc->mask & PROPF_WRITABLE) && (desc->flags & PROPF_WRITABLE))
                    return throw_error(obj->ctx, JS_E_NONWRITABLE_MODIFIED, name);
                if(desc->explicit_value) {
                    if(prop->type == PROP_JSVAL) {
                        BOOL eq;
                        hres = jsval_strict_equal(desc->value, prop->u.val, &eq);
                        if(FAILED(hres))
                            return hres;
                        if(!eq)
                            return throw_error(obj->ctx, JS_E_NONWRITABLE_MODIFIED, name);
                    }else {
                        FIXME("redefinition of property type %d\n", prop->type);
                    }
                }
            }
            if(desc->explicit_value) {
                if(prop->type == PROP_JSVAL)
                    jsval_release(prop->u.val);
                else
                    prop->type = PROP_JSVAL;
                hres = jsval_copy(desc->value, &prop->u.val);
                if(FAILED(hres)) {
                    prop->u.val = jsval_undefined();
                    return hres;
                }
            }
        }
    }else if(desc->explicit_getter || desc->explicit_setter) {
        if(prop->type != PROP_ACCESSOR) {
            if(!(prop->flags & PROPF_CONFIGURABLE))
                return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
            if(prop->type == PROP_JSVAL)
                jsval_release(prop->u.val);
            prop->type = PROP_ACCESSOR;
            prop->u.accessor.getter = prop->u.accessor.setter = NULL;
        }else if(!(prop->flags & PROPF_CONFIGURABLE)) {
            if((desc->explicit_getter && desc->getter != prop->u.accessor.getter)
               || (desc->explicit_setter && desc->setter != prop->u.accessor.setter))
                return throw_error(obj->ctx, JS_E_NONCONFIGURABLE_REDEFINED, name);
        }

        if(desc->explicit_getter) {
            if(prop->u.accessor.getter) {
                jsdisp_release(prop->u.accessor.getter);
                prop->u.accessor.getter = NULL;
            }
            if(desc->getter)
                prop->u.accessor.getter = jsdisp_addref(desc->getter);
        }
        if(desc->explicit_setter) {
            if(prop->u.accessor.setter) {
                jsdisp_release(prop->u.accessor.setter);
                prop->u.accessor.setter = NULL;
            }
            if(desc->setter)
                prop->u.accessor.setter = jsdisp_addref(desc->setter);
        }
    }

    prop->flags = (prop->flags & ~desc->mask) | (desc->flags & desc->mask);
    return S_OK;
}

HRESULT jsdisp_define_data_property(jsdisp_t *obj, const WCHAR *name, unsigned flags, jsval_t value)
{
    property_desc_t prop_desc = { flags, flags, TRUE };
    prop_desc.value = value;
    return jsdisp_define_property(obj, name, &prop_desc);
}

HRESULT jsdisp_change_prototype(jsdisp_t *obj, jsdisp_t *proto)
{
    jsdisp_t *iter;
    DWORD i;

    if(obj->prototype == proto)
        return S_OK;
    if(!obj->extensible)
        return JS_E_CANNOT_CREATE_FOR_NONEXTENSIBLE;

    for(iter = proto; iter; iter = iter->prototype)
        if(iter == obj)
            return JS_E_CYCLIC_PROTO_VALUE;

    if(obj->prototype) {
        for(i = 0; i < obj->prop_cnt; i++)
            if(obj->props[i].type == PROP_PROTREF)
                obj->props[i].type = PROP_DELETED;
        jsdisp_release(obj->prototype);
    }

    obj->prototype = proto;
    if(proto)
        jsdisp_addref(proto);
    return S_OK;
}

void jsdisp_freeze(jsdisp_t *obj, BOOL seal)
{
    unsigned int i;

    for(i = 0; i < obj->prop_cnt; i++) {
        if(!seal && obj->props[i].type == PROP_JSVAL)
            obj->props[i].flags &= ~PROPF_WRITABLE;
        obj->props[i].flags &= ~PROPF_CONFIGURABLE;
    }

    obj->extensible = FALSE;
}

BOOL jsdisp_is_frozen(jsdisp_t *obj, BOOL sealed)
{
    unsigned int i;

    if(obj->extensible)
        return FALSE;

    for(i = 0; i < obj->prop_cnt; i++) {
        if(obj->props[i].type == PROP_JSVAL) {
            if(!sealed && (obj->props[i].flags & PROPF_WRITABLE))
                return FALSE;
        }else if(obj->props[i].type != PROP_ACCESSOR)
            continue;
        if(obj->props[i].flags & PROPF_CONFIGURABLE)
            return FALSE;
    }

    return TRUE;
}

HRESULT jsdisp_get_prop_name(jsdisp_t *obj, DISPID id, jsstr_t **r)
{
    dispex_prop_t *prop = get_prop(obj, id);

    if(!prop)
        return DISP_E_MEMBERNOTFOUND;

    *r = jsstr_alloc(prop->name);
    return *r ? S_OK : E_OUTOFMEMORY;
}
