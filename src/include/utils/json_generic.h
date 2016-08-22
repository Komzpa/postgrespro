/*-------------------------------------------------------------------------
 *
 * json_generic.h
 *	  Declarations for generic json data type support.
 *
 * Copyright (c) 2014-2016, PostgreSQL Global Development Group
 *
 * src/include/utils/json_generic.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef UTILS_JSON_GENERIC_H
#define UTILS_JSON_GENERIC_H

#define JSON_GENERIC

#include "postgres.h"
#include "access/compression.h"
#include "lib/stringinfo.h"
#include "utils/expandeddatum.h"
#include "utils/jsonb.h"

typedef JsonbPair JsonPair;
typedef JsonbValue JsonValue;
typedef JsonbValueType JsonValueType;
typedef JsonbIteratorToken JsonIteratorToken;

typedef struct JsonContainerOps JsonContainerOps;

typedef struct JsonContainerData
{
	JsonContainerOps   *ops;
	void			   *data;
	void			   *options;
	int					len;
	int					size;
	JsonValueType		type;
} JsonContainerData;

typedef const JsonContainerData JsonContainer;

typedef struct JsonIteratorData JsonIterator;

typedef JsonIteratorToken (*JsonIteratorNextFunc)(JsonIterator **iterator,
												  JsonValue *value,
												  bool skipNested);

struct JsonIteratorData
{
	JsonIterator		   *parent;
	JsonContainer		   *container;
	JsonIteratorNextFunc	next;
};

typedef struct JsonCompressionOptionsOps
{
	Size	(*encodeOptions)(JsonContainer *, void *buf);
	Size	(*decodeOptions)(const void *buf, CompressionOptions *);
	bool	(*optionsAreEqual)(JsonContainer *, CompressionOptions);
} JsonCompressionOptionsOps;

struct JsonContainerOps
{
	JsonCompressionOptionsOps *compressionOps;

	void			(*init)(JsonContainerData *jc, Datum value,
							CompressionOptions options);
	JsonIterator   *(*iteratorInit)(JsonContainer *jc);
	JsonValue	   *(*findKeyInObject)(JsonContainer *object,
									   const JsonValue *key);
	JsonValue	   *(*findValueInArray)(JsonContainer *array,
										const JsonValue *value);
	JsonValue	   *(*getArrayElement)(JsonContainer *array, uint32 index);
	uint32			(*getArraySize)(JsonContainer *array);
	char		   *(*toString)(StringInfo out, JsonContainer *jc,
								int estimated_len);
};

typedef struct CompressedObject
{
	ExpandedObjectHeader	eoh;
	Datum					compressed;
	CompressionOptions	   	options;
} CompressedObject;

typedef struct Json
{
	CompressedObject	obj;
	JsonContainerData	root;
} Json;

#undef DatumGetJsonb
#undef JsonbGetDatum

#define JsonGetDatum(json)			EOHPGetRODatum(&(json)->obj.eoh)
#define DatumGetJsont(datum)		DatumGetJson(datum, &jsontContainerOps, NULL)
#define DatumGetJsonb(datum)		DatumGetJson(datum, &jsonbContainerOps, NULL)
#define JsonbGetDatum(json)			JsonGetDatum(json)

#define JsonRoot(json)				(&(json)->root)
#define JsonGetSize(json)			(JsonRoot(json)->len)
#undef JsonbRoot
#undef JsonbGetSize
#define JsonbRoot(json)				JsonRoot(json)
#define JsonbGetSize(json)			JsonGetSize(json)

#define JsonContainerIsArray(c)		(((c)->type & ~jbvScalar) == jbvArray) /* FIXME */
#define JsonContainerIsScalar(c)	((c)->type == (jbvArray | jbvScalar)) /* FIXME */
								/*	((c)-type != jbvObject && (c)->type != jbvArray) */
#define JsonContainerIsObject(c)	((c)->type == jbvObject)
#define JsonContainerSize(c)		((c)->size)
#define JsonContainerIsEmpty(c)		((c)->size == 0)

#define JsonValueIsScalar(jsval)	IsAJsonbScalar(jsval)

#ifdef JSONB_UTIL_C
#define JsonbValueToJsonb JsonValueToJsonb
#else
#define Jsonb Json
#define JsonbIterator JsonIterator
#define JsonbContainer JsonContainer
#define JsonbIteratorInit JsonIteratorInit
#define JsonbIteratorNext JsonIteratorNext
#define JsonbValueToJsonb JsonValueToJson

#undef JB_ROOT_COUNT
#undef JB_ROOT_IS_SCALAR
#undef JB_ROOT_IS_OBJECT
#undef JB_ROOT_IS_ARRAY
#define JB_ROOT_COUNT(json)		JsonContainerSize(JsonRoot(json))
#define JB_ROOT_IS_SCALAR(json)	JsonContainerIsScalar(JsonRoot(json))
#define JB_ROOT_IS_OBJECT(json)	JsonContainerIsObject(JsonRoot(json))
#define JB_ROOT_IS_ARRAY(json)	JsonContainerIsArray(JsonRoot(json))
#endif

#define JsonOp(op, jscontainer) \
		(*(jscontainer)->ops->op)

#define JsonOp0(op, jscontainer) \
		JsonOp(op, jscontainer)(jscontainer)

#define JsonOp1(op, jscontainer, arg) \
		JsonOp(op, jscontainer)(jscontainer, arg)

#define JsonIteratorInit(jscontainer) \
		JsonOp0(iteratorInit, jscontainer)

#define JsonFindValueInArray(jscontainer, key) \
		JsonOp1(findValueInArray, jscontainer, key)

#define JsonFindKeyInObject(jscontainer, key) \
		JsonOp1(findKeyInObject, jscontainer, key)

#define JsonGetArrayElement(jscontainer, index) \
		JsonOp1(getArrayElement, jscontainer, index)

#define JsonGetArraySize(json) \
		JsonOp0(getArraySize, json)

#define JsonIteratorNext(it, val, skip) \
		((*it) ? (*(*(it))->next)(it, val, skip) : WJB_DONE)

#define getIthJsonbValueFromContainer	JsonGetArrayElement
#define findJsonbValueFromContainer		JsonFindValueInContainer
#define findJsonbValueFromContainerLen	JsonFindValueInContainerLen
#define compareJsonbContainers			JsonCompareContainers
#define equalsJsonbScalarValue			JsonValueScalarEquals

extern Json *DatumGetJson(Datum val, JsonContainerOps *ops,
						  CompressionOptions options);

extern JsonValue *JsonFindValueInContainer(JsonContainer *json, uint32 flags,
										   JsonValue *key);

static inline JsonValue *
JsonFindValueInContainerLen(JsonContainer *json, uint32 flags,
							const char *key, uint32 keylen)
{
	JsonValue	k;

	k.type = jbvString;
	k.val.string.val = key;
	k.val.string.len = keylen;

	return JsonFindValueInContainer(json, flags, &k);
}

static inline JsonIterator *
JsonIteratorFreeAndGetParent(JsonIterator *it)
{
	JsonIterator *parent = it->parent;
	pfree(it);
	return parent;
}

extern Json *JsonValueToJson(JsonValue *val);
extern JsonValue *JsonToJsonValue(Json *json, JsonValue *jv);
extern JsonValue *JsonValueUnpackBinary(const JsonValue *jbv);
extern JsonContainer *JsonValueToContainer(const JsonValue *val);

extern int JsonCompareContainers(JsonContainer *a, JsonContainer *b);

extern bool JsonbDeepContains(JsonIterator **val, JsonIterator **mContained);

extern JsonValue *JsonContainerExtractKeys(JsonContainer *jsc);

/* jsonb.c support functions */
extern JsonValue *JsonValueFromCString(char *json, int len);


extern char *JsonbToCString(StringInfo out, JsonContainer *in,
			   int estimated_len);
extern char *JsonbToCStringIndent(StringInfo out, JsonContainer *in,
					 int estimated_len);
extern char *JsonbToCStringCanonical(StringInfo out, JsonContainer *in,
					 int estimated_len);

#define JsonToCString(jc)	JsonToCStringExt(NULL, jc, (jc)->len)

#define JsonToCStringExt(out, in, estimated_len) \
	((*(in)->ops->toString)(out, in, estimated_len))

extern JsonValue   *jsonFindKeyInObject(JsonContainer *obj, const JsonValue *key);
extern JsonValue   *jsonFindValueInArray(JsonContainer *array, const JsonValue *elem);
extern uint32		jsonGetArraySize(JsonContainer *array);
extern JsonValue   *jsonGetArrayElement(JsonContainer *array, uint32 index);

extern bool JsonValueScalarEquals(const JsonValue *aScalar,
								  const JsonValue *bScalar);

typedef void (*JsonValueEncoder)(StringInfo, const JsonValue *,
								 CompressionOptions);

extern void *JsonValueFlatten(const JsonValue *val, JsonValueEncoder encoder,
							  JsonContainerOps *ops, CompressionOptions opts);

extern void JsonbEncode(StringInfo, const JsonValue *, CompressionOptions);
extern void JsonbcEncode(StringInfo, const JsonValue *, CompressionOptions);

#define JsonValueToJsonb(val) \
		JsonValueFlatten(val, JsonbEncode, &jsonbContainerOps, NULL)

#define JsonValueToJsonbc(val, options) \
		JsonValueFlatten(val, JsonbcEncode, &jsonbcContainerOps, options)

extern int lengthCompareJsonbStringValue(const void *a, const void *b);

extern JsonContainerOps jsonbContainerOps;
extern JsonContainerOps jsontContainerOps;

#endif /* UTILS_JSON_GENERIC_H */
