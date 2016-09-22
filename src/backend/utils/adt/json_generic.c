/*
 * json_generic.c
 *
 * Copyright (c) 2014-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/json_generic.c
 *
 */

#include "postgres.h"
#include "miscadmin.h"
#include "utils/json_generic.h"
#include "utils/memutils.h"
#include "utils/builtins.h"

static Json *JsonExpand(Datum value, JsonContainerOps *ops,
						CompressionOptions opts, Json *tmp);

JsonContainerOps jsonvContainerOps;

JsonValue *
JsonValueCopy(JsonValue *res, const JsonValue *val)
{
	check_stack_depth();

	if (!res)
		res = (JsonValue *) palloc(sizeof(JsonValue));

	res->type = val->type;

	switch (val->type)
	{
		case jbvNull:
			break;

		case jbvBool:
			res->val.boolean = val->val.boolean;
			break;

		case jbvString:
		{	/* copy string values in the current context */
			char *buf = palloc(val->val.string.len + 1);
			memcpy(buf, val->val.string.val, val->val.string.len);
			buf[val->val.string.len] = 0;
			res->val.string.val = buf;
			res->val.string.len = val->val.string.len;
			break;
		}

		case jbvNumeric:
			/* same for numeric */
			res->val.numeric =
					DatumGetNumeric(DirectFunctionCall1(numeric_uplus,
											NumericGetDatum(val->val.numeric)));
			break;

		case jbvArray:
		{
			int i;

			res->val.array = val->val.array;
			res->val.array.elems = (JsonValue *)
							palloc(sizeof(JsonValue) * val->val.array.nElems);

			for (i = 0; i < val->val.array.nElems; i++)
				JsonValueCopy(&res->val.array.elems[i],
							  &val->val.array.elems[i]);

			break;
		}

		case jbvObject:
		{
			int i;

			res->val.object = val->val.object;
			res->val.object.pairs = (JsonPair *)
							palloc(sizeof(JsonPair) * val->val.object.nPairs);

			for (i = 0; i < val->val.object.nPairs; i++)
			{
				res->val.object.pairs[i].order = val->val.object.pairs[i].order;
				JsonValueCopy(&res->val.object.pairs[i].key,
							  &val->val.object.pairs[i].key);
				JsonValueCopy(&res->val.object.pairs[i].value,
							  &val->val.object.pairs[i].value);
			}

			break;
		}

		case jbvBinary:
			res->val.binary = val->val.binary;
			res->val.binary.data = JsonCopy(val->val.binary.data);
			break;

		default:
			elog(ERROR, "unknown json value type %d", val->type);
	}

	return res;
}

JsonValue *
JsonExtractScalar(JsonContainer *jc, JsonValue *scalar)
{
	JsonIterator   *it;
	JsonValue		val;

	Assert(JsonContainerIsScalar(jc));

	it = JsonIteratorInit(jc);

	if (JsonIteratorNext(&it, &val, false) != WJB_BEGIN_ARRAY ||
		JsonIteratorNext(&it, scalar, false) != WJB_ELEM ||
		JsonIteratorNext(&it, &val, false) != WJB_END_ARRAY)
		elog(ERROR, "unexpected structure of scalar json container");

	return scalar;
}

const JsonValue *
JsonValueUnwrap(const JsonValue *val, JsonValue *valbuf)
{
	if (val->type == jbvBinary)
	{
		JsonContainer *jc = val->val.binary.data;

		if (jc->ops == &jsonvContainerOps)
		{
			val = (JsonbValue *) jc->data;
			Assert(val->type != jbvBinary);
		}
		else if (JsonContainerIsScalar(jc))
		{
			val = JsonExtractScalar(jc, valbuf);
			Assert(IsAJsonbScalar(val));
		}
	}

	if (val->type == jbvArray && val->val.array.rawScalar)
	{
		val = &val->val.array.elems[0];
		Assert(IsAJsonbScalar(val));
	}

	return val;
}

JsonValue *
JsonValueWrapInBinary(const JsonValue *val, JsonValue *bin)
{
	JsonContainer *jc = JsonValueToContainer(val);

	if (!bin)
		bin = (JsonValue *) palloc(sizeof(JsonValue));

	bin->type = jbvBinary;
	bin->val.binary.data = jc;
	bin->val.binary.len = jc->len;
	bin->val.binary.uniquified = JsonValueIsUniquified(val);

	return bin;
}

JsonValue *
jsonFindKeyInObject(JsonContainer *obj, const JsonValue *key)
{
	JsonValue		   *val = palloc(sizeof(JsonValue));
	JsonIterator	   *it;
	JsonIteratorToken	tok;

	Assert(JsonContainerIsObject(obj));
	Assert(key->type == jbvString);

	it = JsonIteratorInit(obj);

	while ((tok = JsonIteratorNext(&it, val, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY && !lengthCompareJsonbStringValue(key, val))
		{
			tok = JsonIteratorNext(&it, val, true);
			Assert(tok == WJB_VALUE);
			JsonIteratorFree(it);
			return val;
		}
	}

	pfree(val);
	return NULL;
}

JsonValue *
jsonFindValueInArray(JsonContainer *array, const JsonValue *elem)
{
	JsonValue		   *val = palloc(sizeof(JsonValue));
	JsonIterator	   *it;
	JsonIteratorToken	tok;

	Assert(JsonContainerIsArray(array));
	Assert(IsAJsonbScalar(elem));

	it = JsonIteratorInit(array);

	while ((tok = JsonIteratorNext(&it, val, true)) != WJB_DONE)
	{
		if (tok == WJB_ELEM && val->type == elem->type &&
			equalsJsonbScalarValue(val, elem))
		{
			JsonIteratorFree(it);
			return val;
		}
	}

	pfree(val);
	return NULL;
}

JsonValue *
jsonGetArrayElement(JsonContainer *array, uint32 index)
{
	JsonValue		   *val = palloc(sizeof(JsonValue));
	JsonIterator	   *it;
	JsonIteratorToken	tok;

	Assert(JsonContainerIsArray(array));

	it = JsonIteratorInit(array);

	while ((tok = JsonIteratorNext(&it, val, true)) != WJB_DONE)
	{
		if (tok == WJB_ELEM)
		{
			if (index-- == 0)
			{
				JsonIteratorFree(it);
				return val;
			}
		}
	}

	pfree(val);

	return NULL;
}

uint32
jsonGetArraySize(JsonContainer *array)
{
	JsonValue		    val;
	JsonIterator	   *it;
	JsonIteratorToken	tok;
	uint32				size = 0;

	Assert(JsonContainerIsArray(array));

	it = JsonIteratorInit(array);

	while ((tok = JsonIteratorNext(&it, &val, true)) != WJB_DONE)
	{
		if (tok == WJB_ELEM)
			size++;
	}

	return size;
}

static void
jsonvInitContainer(JsonContainerData *jc, const JsonValue *val)
{
	jc->ops = &jsonvContainerOps;
	jc->data = (void *) val;
	jc->len = 0;
	jc->size = val->type == jbvBinary ? val->val.binary.data->size :
			   val->type == jbvObject ? val->val.object.nPairs :
			   val->type == jbvArray  ? val->val.array.nElems : 1;
	jc->type = val->type == jbvBinary ? val->val.binary.data->type :
			   val->type == jbvObject ? jbvObject :
			   val->type == jbvArray && !val->val.array.rawScalar ? jbvArray :
										jbvArray | jbvScalar;
}

JsonContainer *
JsonValueToContainer(const JsonValue *val)
{
	if (val->type == jbvBinary)
		return val->val.binary.data;
	else
	{
		JsonContainerData *jc = JsonContainerAlloc();
		jsonvInitContainer(jc, val);
		return jc;
	}
}

typedef struct JsonvScalarIterator
{
	JsonIterator		ji;
	JsonIteratorToken	next;
} JsonvScalarIterator;

typedef struct JsonvArrayIterator
{
	JsonIterator	ji;
	int				index;
} JsonvArrayIterator;

typedef struct JsonvObjectIterator
{
	JsonIterator	ji;
	int				index;
	bool			value;
} JsonvObjectIterator;

static JsonIterator *
jsonvIteratorInitFromValue(JsonValue *val, JsonContainer *jsc);

static JsonIteratorToken
jsonvScalarIteratorNext(JsonIterator **it, JsonValue *res, bool skipNested)
{
	JsonvScalarIterator	*sit = (JsonvScalarIterator *) *it;
	JsonValue			*val = (*it)->container->data;

	Assert(IsAJsonbScalar(val));

	switch (sit->next)
	{
		case WJB_BEGIN_ARRAY:
			res->type = jbvArray;
			res->val.array.rawScalar = true;
			res->val.array.nElems = 1;
			res->val.array.elems = NULL;
			sit->next = WJB_ELEM;
			return WJB_BEGIN_ARRAY;

		case WJB_ELEM:
			*res = *val;
			sit->next = WJB_END_ARRAY;
			return WJB_ELEM;

		case WJB_END_ARRAY:
			sit->next = WJB_DONE;
			*it = JsonIteratorFreeAndGetParent(*it);
			return WJB_END_ARRAY;

		default:
			return WJB_DONE;
	}
}

static JsonIteratorToken
jsonvArrayIteratorNext(JsonIterator **it, JsonValue *res, bool skipNested)
{
	JsonvArrayIterator	*ait = (JsonvArrayIterator *) *it;
	JsonValue			*arr = (*it)->container->data;
	JsonValue			*val;

	Assert(arr->type == jbvArray);

	if (ait->index == -1)
	{
		ait->index = 0;
		*res = *arr;
		return WJB_BEGIN_ARRAY;
	}

	if (ait->index >= arr->val.array.nElems)
	{
		*it = JsonIteratorFreeAndGetParent(*it);
		return WJB_END_ARRAY;
	}

	val = &arr->val.array.elems[ait->index++]; /* FIXME palloced copy */
	*res = *val;

	if (!IsAJsonbScalar(res))
	{
		if (!skipNested)
		{
			JsonIterator *child = jsonvIteratorInitFromValue(val, NULL);
			child->parent = *it;
			*it = child;
			return WJB_RECURSE;
		}
		else if (res->type != jbvBinary)
		{
			Assert(res->type == jbvArray || res->type == jbvObject);
			res->val.binary.data = JsonValueToContainer(val);
			res->val.binary.len = 0;
			res->val.binary.uniquified = JsonValueIsUniquified(val);
			res->type = jbvBinary;
		}
	}

	return WJB_ELEM;
}

static JsonIteratorToken
jsonvObjectIteratorNext(JsonIterator **it, JsonValue *res, bool skipNested)
{
	JsonvObjectIterator	*oit = (JsonvObjectIterator *) *it;
	JsonValue			*obj = (*it)->container->data;
	JsonPair			*pair;

	Assert(obj->type == jbvObject);

	if (oit->index == -1)
	{
		oit->index = 0;
		*res = *obj;
		return WJB_BEGIN_OBJECT;
	}

	if (oit->index >= obj->val.object.nPairs)
	{
		*it = JsonIteratorFreeAndGetParent(*it);
		return WJB_END_OBJECT;
	}

	pair = &obj->val.object.pairs[oit->index];

	if (oit->value)
	{
		*res = pair->value;
		oit->value = false;
		oit->index++;

		if (!IsAJsonbScalar(res))
		{
			if (!skipNested)
			{
				JsonIterator *chld =
						jsonvIteratorInitFromValue(&pair->value, NULL);
				chld->parent = *it;
				*it = chld;
				return WJB_RECURSE;
			}
			else if (res->type != jbvBinary)
			{
				Assert(res->type == jbvArray || res->type == jbvObject);
				res->val.binary.data = JsonValueToContainer(&pair->value);
				res->val.binary.len = 0;
				res->val.binary.uniquified =
											JsonValueIsUniquified(&pair->value);
				res->type = jbvBinary;
			}
		}

		return WJB_VALUE;
	}
	else
	{
		*res = pair->key;
		oit->value = true;
		return WJB_KEY;
	}
}

static JsonIterator *
JsonIteratorCreate(Size size, JsonContainer *jsc, JsonIteratorNextFunc next)
{
	JsonIterator *it = (JsonIterator *) palloc(size);

	it->container = jsc;
	it->parent = NULL;
	it->next = next;

	return it;
}

static JsonIterator *
JsonvArrayIteratorInit(JsonValue *val, JsonContainer *jsc)
{
	JsonvArrayIterator *it = (JsonvArrayIterator *)
			JsonIteratorCreate(sizeof(JsonvArrayIterator),
							   jsc ? jsc : JsonValueToContainer(val),
							   jsonvArrayIteratorNext);
	it->index = -1;

	return &it->ji;

}

static JsonIterator *
JsonvObjectIteratorInit(JsonValue *val, JsonContainer *jsc)
{
	JsonvObjectIterator *it = (JsonvObjectIterator *)
			JsonIteratorCreate(sizeof(JsonvObjectIterator),
							   jsc ? jsc : JsonValueToContainer(val),
							   jsonvObjectIteratorNext);
	it->index = -1;
	it->value = false;

	return &it->ji;
}

static JsonIterator *
JsonvScalarIteratorInit(JsonValue *val, JsonContainer *jsc)
{
	JsonvScalarIterator *it = (JsonvScalarIterator *)
			JsonIteratorCreate(sizeof(JsonvScalarIterator),
							   jsc ? jsc : JsonValueToContainer(val),
							   jsonvScalarIteratorNext);

	it->next = WJB_BEGIN_ARRAY;

	return &it->ji;
}

static JsonIterator *
jsonvIteratorInitFromValue(JsonValue *val, JsonContainer *jsc)
{
	if (val->type == jbvObject)
		return JsonvObjectIteratorInit(val, jsc);
	else if (val->type == jbvArray)
		return JsonvArrayIteratorInit(val, jsc);
	else if (val->type == jbvBinary)
		return JsonIteratorInit(val->val.binary.data);
	else if (IsAJsonbScalar(val))
		return JsonvScalarIteratorInit(val, jsc);
	else
	{
		elog(ERROR, "unexpected json value container type: %d", val->type);
		return NULL;
	}
}

static JsonIterator *
jsonvIteratorInit(JsonContainer *jsc)
{
	return jsonvIteratorInitFromValue(jsc->data, jsc);
}

static JsonValue *
jsonvFindKeyInObject(JsonContainer *objc, const JsonValue *key)
{
	JsonValue  *obj = (JsonValue *) objc->data;
	int			i;

	Assert(JsonContainerIsObject(objc));
	Assert(key->type == jbvString);

	if (obj->type == jbvBinary)
	{
		JsonContainer *jsc = obj->val.binary.data;
		Assert(jsc->type == jbvObject);
		return (*jsc->ops->findKeyInObject)(jsc, key);
	}

	Assert(obj->type == jbvObject);

	for (i = 0; i < obj->val.object.nPairs; i++)
	{
		JsonPair *pair = &obj->val.object.pairs[i];
		if (!lengthCompareJsonbStringValue(key, &pair->key))
		{

			return &pair->value; /* FIXME palloced copy */
		}
	}

	return NULL;
}

static JsonValue *
jsonvFindValueInArray(JsonContainer *arrc, const JsonValue *val)
{
	JsonValue  *arr = (JsonValue *) arrc->data;
	int			i;

	Assert(JsonContainerIsArray(arrc));
	Assert(IsAJsonbScalar(val));

	if (arr->type == jbvBinary)
	{
		JsonContainer *jsc = arr->val.binary.data;
		Assert(jsc->type == jbvArray);
		return (*jsc->ops->findValueInArray)(jsc, val);
	}

	Assert(arr->type == jbvArray);

	for (i = 0; i < arr->val.array.nElems; i++)
	{
		JsonValue *elem = &arr->val.array.elems[i];
		if (val->type == elem->type && equalsJsonbScalarValue(val, elem))
			return elem; /* FIXME palloced copy */
	}

	return NULL;
}

static JsonValue *
jsonvGetArrayElement(JsonContainer *arrc, uint32 index)
{
	JsonValue  *arr = (JsonValue *) arrc->data;

	Assert(JsonContainerIsArray(arrc));

	if (arr->type == jbvBinary)
	{
		JsonContainer *jsc = arr->val.binary.data;
		Assert(jsc->type == jbvArray);
		return (*jsc->ops->getArrayElement)(jsc, index);
	}

	Assert(arr->type == jbvArray);

	if (index >= arr->val.array.nElems)
		return NULL;

	return &arr->val.array.elems[index]; /* FIXME palloced copy */
}

static uint32
jsonvGetArraySize(JsonContainer *arrc)
{
	JsonValue  *arr = (JsonValue *) arrc->data;

	Assert(JsonContainerIsArray(arrc));

	if (arr->type == jbvBinary)
	{
		JsonContainer *jsc = arr->val.binary.data;
		Assert(jsc->type == jbvArray);
		if (jsc->size >= 0)
			return jsc->size;
		return /* FIXME jsc->size = */ (*jsc->ops->getArraySize)(jsc);
	}

	Assert(arr->type == jbvArray);

	return arr->val.array.nElems;
}

static JsonContainer *
jsonvCopy(JsonContainer *jc)
{
	JsonContainerData *res = JsonContainerAlloc();

	*res = *jc;
	res->data = JsonValueCopy(NULL, (JsonValue *) jc->data);

	return res;
}

JsonContainerOps
jsonvContainerOps =
{
		NULL,
		NULL,
		jsonvIteratorInit,
		jsonvFindKeyInObject,
		jsonvFindValueInArray,
		jsonvGetArrayElement,
		jsonvGetArraySize,
		JsonbToCStringRaw,
		jsonvCopy,
};

JsonValue *
JsonToJsonValue(Json *json, JsonValue *jv)
{
	if (JsonRoot(json)->ops == &jsonvContainerOps)
		return (JsonValue *) JsonRoot(json)->data;

	if (!jv)
		jv = palloc(sizeof(JsonValue));

	jv->type = jbvBinary;
	jv->val.binary.data = &json->root;
	jv->val.binary.len = json->root.len;
	jv->val.binary.uniquified = json->root.ops != &jsontContainerOps;

	return jv;
}

extern JsonContainerOps jsontContainerOps;
extern JsonContainerOps jsonbContainerOps;
extern JsonContainerOps jsonbcContainerOps;

typedef enum
{
	JsonContainerUnknown,
	JsonContainerJsont,
	JsonContainerJsonb,
	JsonContainerJsonbc,
} JsonContainerType;

typedef struct varatt_extended_json
{
	varatt_extended_hdr vaext;
	char				type;
	char				data[FLEXIBLE_ARRAY_MEMBER];
} varatt_extended_json;

static Size
jsonGetExtendedSize(JsonContainer *jc)
{
	return VARHDRSZ_EXTERNAL + offsetof(varatt_extended_json, data) +
			jc->len + (jc->ops->compressionOps ?
						jc->ops->compressionOps->encodeOptions(jc, NULL) : 0);
}

#define JsonContainerGetType(jc) \
	((jc)->ops == &jsontContainerOps  ? JsonContainerJsont : \
	 (jc)->ops == &jsonbContainerOps  ? JsonContainerJsonb : \
	 (jc)->ops == &jsonbcContainerOps ? JsonContainerJsonbc : \
										JsonContainerUnknown)

#define JsonContainerGetOpsByType(type) \
		((type) == JsonContainerJsont ? &jsontContainerOps : \
		 (type) == JsonContainerJsonb ? &jsonbContainerOps : \
		 (type) == JsonContainerJsonbc ? &jsonbcContainerOps : NULL)

static void
jsonWriteExtended(JsonContainer *jc, void *ptr, Size allocated_size)
{
	varatt_extended_json	extjs,
						   *pextjs;
	Size					optionsSize;

	Assert(allocated_size >= jsonGetExtendedSize(jc));

	extjs.vaext.size = jsonGetExtendedSize(jc) - VARHDRSZ_EXTERNAL;
	extjs.type = JsonContainerGetType(jc);
	Assert(extjs.type != JsonContainerUnknown);

	SET_VARTAG_EXTERNAL(ptr, VARTAG_EXTENDED);
	pextjs = (varatt_extended_json *) VARDATA_EXTERNAL(ptr);
	memcpy(pextjs, &extjs, offsetof(varatt_extended_json, data));

	optionsSize = jc->ops->compressionOps ?
				  jc->ops->compressionOps->encodeOptions(jc, &pextjs->data) : 0;

	memcpy(&pextjs->data[optionsSize], jc->data, jc->len);
}

static void
JsonInitExtended(Json *json)
{
	JsonContainerData	   *jc = JsonRoot(json);
	varatt_extended_json   *pextjs,
							extjs;
	void				   *ptr = DatumGetPointer(json->obj.value);
	int						len;

	Assert(VARATT_IS_EXTERNAL_EXTENDED(ptr));

	pextjs = (varatt_extended_json *) VARDATA_EXTERNAL(ptr);
	memcpy(&extjs, pextjs, offsetof(varatt_extended_json, data));

	jc->ops = JsonContainerGetOpsByType(extjs.type);
	Assert(jc->ops);

	len = extjs.vaext.size - offsetof(varatt_extended_json, data);

#if 1
	{
		/* FIXME save value with varlena header */
		Size	optionsSize =
				jc->ops->compressionOps ?
				jc->ops->compressionOps->decodeOptions(&pextjs->data,
													   &json->obj.options) : 0;
		void   *p = palloc(VARHDRSZ + len);

		SET_VARSIZE(p, VARHDRSZ + len);
		memcpy(VARDATA(p), &pextjs->data[optionsSize], len);
		if (json->obj.freeValue)
			pfree(DatumGetPointer(json->obj.value));
		json->obj.value = PointerGetDatum(p);
		json->obj.freeValue = true;
	}
#else
	jc->len = len;
#if 0 /* FIXME alignment */
	jc->data = &pextjs->data;
#else
	jc->data = palloc(jc->len);
	memcpy(jc->data, &pextjs->data, jc->len);
#endif
#endif
}

static void
JsonInit(Json *json)
{
	const void *data = DatumGetPointer(json->obj.value);
	const void *detoasted_data;

	Assert(json->root.data || data);

	if (json->root.data || !data)
		return;

	detoasted_data = PG_DETOAST_DATUM(json->obj.value);
	json->obj.value = PointerGetDatum(detoasted_data);
	json->obj.freeValue = data != detoasted_data;

	if (VARATT_IS_EXTERNAL_EXTENDED(detoasted_data))
		JsonInitExtended(json);

	json->root.ops->init(&json->root, json->obj.value, json->obj.options);
}


#define JSON_FLATTEN_INTO_JSONEXT
#define JSON_FLATTEN_INTO_JSONB
#define flatContainerOps &jsonbContainerOps

static Size
jsonGetFlatSizeJsont(Json *json, void **context)
{
	Size	size;
	char   *str = JsonToCString(JsonRoot(json));
	size = VARHDRSZ + strlen(str);
	if (context)
		*context = str;
	else
		pfree(str);
	return size;
}

static void *
jsonFlattenJsont(Json *json, void **context)
{
	char   *str = context ? (char *) *context : JsonToCString(JsonRoot(json));
	text   *text = cstring_to_text(str);
	pfree(str);
	return text;
}

static Size
jsonGetFlatSize2(Json *json, void **context)
{
#ifdef JSON_FLATTEN_INTO_JSONT
	return jsonGetFlatSizeJsont(json, context);
#else
	Size		size;
	JsonValue	val;
# ifdef JSON_FLATTEN_INTO_JSONBC
	void	   *js = JsonValueToJsonbc(JsonToJsonValue(json, &val));
# else
	void	   *js = JsonValueToJsonb(JsonToJsonValue(json, &val));
# endif
	size = VARSIZE(js);
	if (context)
		*context = js;
	else
		pfree(js);
#endif

	return size;
}

static void *
jsonFlatten(Json *json, void **context)
{
#ifdef JSON_FLATTEN_INTO_JSONT
	return jsonFlattenJsont(json, context)
#else
	if (context)
		return *context;
	else
	{
		JsonValue	valbuf;
		JsonValue  *val = JsonToJsonValue(json, &valbuf);
# ifdef JSON_FLATTEN_INTO_JSONBC
		return JsonValueToJsonbc(val);
# else
		return JsonValueToJsonb(val);
# endif
	}
#endif
}

static Size
jsonGetFlatSize(ExpandedObjectHeader *eoh, void **context)
{
	Json   *json = (Json *) eoh;

	JsonInit(json);

#ifdef JSON_FLATTEN_INTO_JSONEXT
	{
		JsonContainer	   *flat = JsonRoot(json);
		JsonContainerData	tmp;

		if (json->root.ops == &jsonvContainerOps)
		{
			JsonValue *val = (JsonValue *) flat->data;

			if (JsonValueIsUniquified(val))
			{
				tmp.len = jsonGetFlatSize2(json, context) - VARHDRSZ;
				tmp.ops = flatContainerOps;
			}
			else
			{
				tmp.len = jsonGetFlatSizeJsont(json, context) - VARHDRSZ;
				tmp.ops = &jsontContainerOps;
			}

			tmp.data = NULL;

			flat = &tmp;
		}

		return jsonGetExtendedSize(flat);
	}
#else
	return jsonGetFlatSize2(json, context);
#endif
}

static void
jsonFlattenInto(ExpandedObjectHeader *eoh, void *result, Size allocated_size,
				void **context)
{
	Json   *json = (Json *) eoh;

	JsonInit(json);

#ifdef JSON_FLATTEN_INTO_JSONEXT
	{
		JsonContainer	   *flat = JsonRoot(json);
		JsonContainerData	tmp;
		void			   *tmpData = NULL;

		if (flat->ops == &jsonvContainerOps)
		{
			JsonValue *val = (JsonValue *) flat->data;

			if (JsonValueIsUniquified(val))
			{
				tmpData = jsonFlatten(json, context);
				tmp.ops = flatContainerOps;
			}
			else
			{
				tmpData = jsonFlattenJsont(json, context);
				tmp.ops = &jsontContainerOps;
			}

			tmp.data = VARDATA(tmpData);
			tmp.len = VARSIZE(tmpData) - VARHDRSZ;

			flat = &tmp;
		}

		jsonWriteExtended(flat, result, allocated_size);

		if (tmpData)
			pfree(tmpData);
	}
#else
	{
		void *data = jsonFlatten(json, context);
		memcpy(result, data, allocated_size);
		pfree(data);
	}
#endif
}

static ExpandedObjectMethods
jsonExpandedObjectMethods =
{
	jsonGetFlatSize,
	jsonFlattenInto
};

static Json *
JsonExpand(Datum value, JsonContainerOps *ops, CompressionOptions options,
		   Json *tmp)
{
	MemoryContext	objcxt;
	Json		   *json;

	if (tmp)
	{
		json = tmp;
		json->obj.eoh.vl_len_ = 0;
	}
	else
	{
#ifndef JSON_EXPANDED_OBJECT_MCXT
		json = (Json *) palloc(sizeof(Json));
		objcxt = NULL;
#else
		/*
		 * Allocate private context for expanded object.  We start by assuming
		 * that the json won't be very large; but if it does grow a lot, don't
		 * constrain aset.c's large-context behavior.
		 */
		objcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "expanded json",
									   ALLOCSET_SMALL_MINSIZE,
									   ALLOCSET_SMALL_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);

		json = (Json *) MemoryContextAlloc(objcxt, sizeof(Json));
#endif

		EOH_init_header(&json->obj.eoh, &jsonExpandedObjectMethods, objcxt);
	}

	json->obj.value = value;
	json->obj.options = options;
	json->root.data = NULL;
	json->root.len = 0;
	json->root.ops = ops;
	json->root.size = -1;
	json->root.type = jbvBinary;

	return json;
}

static Json *
JsonExpandDatum(Datum value, JsonContainerOps *ops, CompressionOptions options,
				Json *tmp)
{
	return VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value))
					? (Json *) DatumGetEOHP(value)
					: JsonExpand(value, ops, options, tmp);
}

Json *
DatumGetJson(Datum value, JsonContainerOps *ops, CompressionOptions options,
			 Json *tmp)
{
	Json *json = JsonExpandDatum(value, ops, options, tmp);
	JsonInit(json);
	return json;
}

void
JsonFree(Json *json)
{
	if (json->obj.freeValue)
		pfree(DatumGetPointer(json->obj.value));

	if (!JsonIsTemporary(json))
		pfree(json);
}

Json *
JsonCopyTemporary(Json *tmp)
{
	Json *json = (Json *) palloc(sizeof(Json));

	memcpy(json, tmp, sizeof(Json));
	tmp->obj.freeValue = false;

	EOH_init_header(&json->obj.eoh, &jsonExpandedObjectMethods, NULL);

	return json;
}

Json *
JsonValueToJson(JsonValue *val)
{
	if (val->type == jbvBinary)
	{
		JsonContainer  *jc = val->val.binary.data;
		Json		   *json = JsonExpand(PointerGetDatum(NULL), jc->ops, NULL,
										  NULL);
		json->root = *jc;
		return json;
	}
	else
	{
		Json *json = JsonExpand(PointerGetDatum(NULL), &jsonvContainerOps, NULL,
								NULL);
		jsonvInitContainer(&json->root, val);
		return json;
	}
}

JsonContainer *
JsonCopyFlat(JsonContainer *jc)
{
	JsonContainerData *res = JsonContainerAlloc();

	*res = *jc;
	res->data = palloc(jc->len);
	memcpy(res->data, jc->data, jc->len);

	if (jc->ops->compressionOps && jc->ops->compressionOps->copyOptions)
		res->options = jc->ops->compressionOps->copyOptions(jc->options);

	return res;
}

JsonValue *
JsonContainerExtractKeys(JsonContainer *jsc)
{
	JsonIterator	   *it;
	JsonbParseState	   *state = NULL;
	JsonValue		   *res = NULL;
	JsonValue			val;
	JsonIteratorToken	tok;

	Assert(JsonContainerIsObject(jsc));

	it = JsonIteratorInit(jsc);

	while ((tok = JsonIteratorNext(&it, &val, false)) != WJB_DONE)
	{
		res = pushJsonbValue(&state, tok, tok < WJB_BEGIN_ARRAY ? &val : NULL);

		if (tok == WJB_KEY)
		{
			tok = JsonIteratorNext(&it, &val, true);
			Assert(tok == WJB_VALUE);
			pushJsonbValueScalar(&state, tok, &val);
		}
	}

	return res;
}