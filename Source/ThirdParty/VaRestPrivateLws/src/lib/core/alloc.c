#include "core/private.h"

#if defined(LWS_PLAT_OPTEE)

#define TEE_USER_MEM_HINT_NO_FILL_ZERO       0x80000000

void *__attribute__((weak))
	TEE_Malloc(uint32_t size, uint32_t hint)
{
	return NULL;
}
void *__attribute__((weak))
	TEE_Realloc(void *buffer, uint32_t newSize)
{
	return NULL;
}
void __attribute__((weak))
	TEE_Free(void *buffer)
{
}

void *lws_realloc(void *ptr, size_t size, const char *reason)
{
	return TEE_Realloc(ptr, size);
}

void *lws_malloc(size_t size, const char *reason)
{
	return TEE_Malloc(size, TEE_USER_MEM_HINT_NO_FILL_ZERO);
}

void lws_free(void *p)
{
	TEE_Free(p);
}

void *lws_zalloc(size_t size, const char *reason)
{
	void *ptr = TEE_Malloc(size, TEE_USER_MEM_HINT_NO_FILL_ZERO);
	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

void lws_set_allocator(void *(*cb)(void *ptr, size_t size, const char *reason))
{
	(void)cb;
}
#else

static void *_realloc(void *ptr, size_t size, const char *reason)
{
	if (size) {
#if defined(LWS_WITH_ESP32)
		lwsl_notice("%s: size %lu: %s (free heap %d)\n", __func__,
			    (unsigned long)size, reason, (unsigned int)esp_get_free_heap_size() - (int)size);
#else
		lwsl_debug("%s: size %lu: %s\n", __func__,
			   (unsigned long)size, reason);
#endif
#if defined(LWS_PLAT_OPTEE)
		return (void *)TEE_Realloc(ptr, size);
#else
		return (void *)realloc(ptr, size);
#endif
	}
	if (ptr)
		free(ptr);

	return NULL;
}

void *(*_lws_realloc)(void *ptr, size_t size, const char *reason) = _realloc;

void *lws_realloc(void *ptr, size_t size, const char *reason)
{
	return _lws_realloc(ptr, size, reason);
}

void *lws_zalloc(size_t size, const char *reason)
{
	void *ptr = _lws_realloc(NULL, size, reason);
	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

void lws_set_allocator(void *(*cb)(void *ptr, size_t size, const char *reason))
{
	_lws_realloc = cb;
}
#endif


/* Contextual libc allocator: no _lws_realloc/global allocator installation.
 * All physical result stores precede any ACK, zero fill, log or callback. */
static uint64_t
vr_allocator_ceiling1(uint32_t kind, uint32_t sub)
{
 if (kind == 7 && sub == 1) return 65536;
 if (kind == 6 && sub == 1) return 16384;
 if (kind == 5 && (sub == 1 || sub == 2)) return 16384;
 if (kind != 4) return 0;
 switch (sub) {
 case 2: return 32768; case 3: return 24; case 4: return 512;
 case 5: return 8; case 6: return 4096; case 7: return 65556;
 case 8: return 256; case 9: case 10: return 4096;
 case 11: return 65552; case 12: return 32800;
 case 13: return 8192; case 14: return 32768; case 15: return 65536;
 default: return 0;
 }
}

int
lws_vr_allocate1(const VrLwsCustody1 *view, const VrLwsRowRef1 *parent,
 uint32_t kind, uint32_t sub, uint64_t bytes, const VrLwsBudgetRef1 *budget,
 struct lws_vr_resource1 *r, uint32_t where)
{
 void *pointer;
 unsigned char *p;
 size_t n;
 int error, status;
 if (!bytes || bytes > SIZE_MAX || bytes > vr_allocator_ceiling1(kind, sub))
  return VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
 status = lws_vr_reserve1(view, parent, kind, sub, budget, r);
 if (status != VR_LWS_KNOWN_SUCCESS1)
  return status;
 r->Bytes = bytes; /* exact requested Q prepared before the actual call */
 status = lws_vr_debit1(view, budget, r, VR_LWS_NAMED_ACQUIRE1);
 if (status != VR_LWS_KNOWN_SUCCESS1) {
  int never = lws_vr_never1(view, budget, r, where);
  return never == VR_LWS_RELEASE_UNCERTAIN1 ? never : status;
 }
 pointer = malloc((size_t)bytes);
 error = pointer ? 0 : errno;
 r->Pointer = pointer;
 r->Stage = pointer ? LWS_VR_ACQUIRED1 : LWS_VR_NO_RESOURCE1;
 /* No fallible operation between real return/capture and this pure record. */
 varest_ws_c2l_record_pointer_acquire1(view, &r->Row, pointer, bytes, error, where);
 if (!pointer) {
  status = lws_vr_ack_retire1(view, budget, r);
  return status == VR_LWS_KNOWN_SUCCESS1 ? VR_LWS_NO_RESOURCE_FAILURE1 : status;
 }
 /* Captured ownership exists before zero-fill; no constructor/allocator hook. */
 p = pointer;
 for (n = 0; n < (size_t)bytes; ++n)
  p[n] = 0;
 return lws_vr_ack_retire1(view, budget, r);
}

int
lws_vr_free1(const VrLwsCustody1 *view, const VrLwsBudgetRef1 *budget,
 struct lws_vr_resource1 *r, uint32_t where)
{
 void *pointer;
 uint64_t bytes;
 int status;
 if (r->Stage == LWS_VR_EMPTY1 || r->Stage == LWS_VR_RETIRED1)
  return VR_LWS_KNOWN_SUCCESS1;
 if (r->Stage == LWS_VR_FREE_ENTERED1)
  return VR_LWS_RELEASE_UNCERTAIN1; /* no retry or invented return */
 if (r->Stage == LWS_VR_RESERVED1)
  return lws_vr_never1(view, budget, r, where);
 if (r->Stage == LWS_VR_NO_RESOURCE1 || r->Stage == LWS_VR_FREE_RETURNED1 ||
     r->Stage == LWS_VR_UNKNOWN1 || r->Stage == LWS_VR_DISPOSE_UNKNOWN1 ||
     r->Stage == LWS_VR_UNKNOWN_ACK1)
  return lws_vr_ack_retire1(view, budget, r);
 if (r->Stage != LWS_VR_ACQUIRED1 || !r->Pointer ||
     !r->Bytes || r->Bytes > vr_allocator_ceiling1(r->Kind, r->Subkind))
  return VR_LWS_PROTOCOL_FAULT1;
 status = lws_vr_debit1(view, budget, r, VR_LWS_NAMED_DISPOSE1);
 if (status != VR_LWS_KNOWN_SUCCESS1)
  return status;
 pointer = r->Pointer; bytes = r->Bytes;
 r->Stage = LWS_VR_FREE_ENTERED1; /* one-shot intent; no disposer retry */
 free(pointer);
 r->Stage = LWS_VR_FREE_RETURNED1;
 varest_ws_c2l_record_void_dispose_return1(view, &r->Row, pointer, bytes, where);
 /* r must be genuine EXTERNAL storage if pointer contains this resource.
  * The context/vhost/event free tails explicitly copy only this metadata out
  * before free, and never access that freed object afterward. */
 return lws_vr_ack_retire1(view, budget, r);
}
