/* Copyright 2014-2026 Vladimir Alyamkin.
 * VaRest modifications to libwebsockets, distributed under the same LGPL-2.1
 * terms as the bundled private library. See its accompanying license. */
/* Source-only contextual custody. Never a global allocator/observer/installer. */
#include "core/private.h"

static int
vr_hook1(const VrLwsCustody1 *v, struct lws_vr_resource1 *r, const VrLwsHookOutcome1 *h)
{
 r->LastHook = *h;
 if (h->NativeStatus == 19) {
  r->ObservedNative19 = 1;
  v->Control->Fault |= VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1;
 }
 if (r->ObservedNative19)
  return VR_LWS_RELEASE_UNCERTAIN1;
 return h->Disposition == VR_LWS_HOOK_COMPLETED1 ? VR_LWS_KNOWN_SUCCESS1 :
  h->Disposition == VR_LWS_HOOK_PROTOCOL_FAULT1 ? VR_LWS_PROTOCOL_FAULT1 :
  VR_LWS_FAILED_WITH_OUTSTANDING1;
}

void
lws_vr_resource_init1(struct lws_vr_resource1 *r)
{
 unsigned char *p = (unsigned char *)r;
 size_t n;
 for (n = 0; n < sizeof(*r); ++n)
  p[n] = 0;
 r->Fd = -1;
}

int
lws_vr_reserve1(const VrLwsCustody1 *v, const VrLwsRowRef1 *parent,
 uint32_t kind, uint32_t sub, const VrLwsBudgetRef1 *b,
 struct lws_vr_resource1 *r)
{
 VrLwsSpec1 spec = {0};
 VrLwsHookOutcome1 h = {VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
 if (!v || !v->ResourceBinding || !v->Control || !b || !r ||
     r->Stage != LWS_VR_EMPTY1 || r->Row.SourceSerial || r->Row.Cell ||
     r->Pointer || r->Bytes || r->Kind || r->Subkind || r->Fd != -1 ||
     r->ObservedNative19 || r->AcquisitionAck || r->TerminalAck || r->Reserved)
  return VR_LWS_PROTOCOL_FAULT1;
 if (v->Control->Fault)
  return v->Control->Fault & VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1 ?
   VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_PROTOCOL_FAULT1;
 /* A new primitive must leave the fixed C1+L2+selected TLS cleanup floor INSIDE the
  * same actual startup gas. No helper initializes, adds or renews gas. */
 if (b->Creating && (b->Creating->Phase != VR_LWS_PHASE_STARTUP1 || b->Creating->Remaining < 512))
  return VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
 varest_ws_c2l_prepare_spec1(v, parent, kind, sub, &spec, &h);
 if (vr_hook1(v, r, &h) != VR_LWS_KNOWN_SUCCESS1)
  return r->ObservedNative19 ? VR_LWS_RELEASE_UNCERTAIN1 :
   (h.Disposition == VR_LWS_HOOK_PROTOCOL_FAULT1 || h.Disposition == VR_LWS_HOOK_LOCAL_INVALID1) ?
    VR_LWS_PROTOCOL_FAULT1 : VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
 if (spec.Kind != kind || spec.Subkind != sub || spec.Cell < 12 ||
     spec.Cell >= 140 || spec.Location != spec.Cell)
  return VR_LWS_PROTOCOL_FAULT1;
 h.Disposition = VR_LWS_HOOK_PROTOCOL_FAULT1;
 varest_ws_c2l_reserve1(v, &spec, b, &r->Row, &h);
 if (vr_hook1(v, r, &h) != VR_LWS_KNOWN_SUCCESS1)
  return r->ObservedNative19 ? VR_LWS_RELEASE_UNCERTAIN1 :
   (h.Disposition == VR_LWS_HOOK_PROTOCOL_FAULT1 || h.Disposition == VR_LWS_HOOK_LOCAL_INVALID1) ?
    VR_LWS_PROTOCOL_FAULT1 : VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
 /* The actual issued source row is durable BEFORE any real primitive. */
 r->Kind = kind; r->Subkind = sub; r->Stage = LWS_VR_RESERVED1;
 return VR_LWS_KNOWN_SUCCESS1;
}

int
lws_vr_debit1(const VrLwsCustody1 *v, const VrLwsBudgetRef1 *b,
 struct lws_vr_resource1 *r, uint32_t operation)
{
 VrLwsHookOutcome1 h = {VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
 if (!r || !b || r->ObservedNative19)
  return r && r->ObservedNative19 ? VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_PROTOCOL_FAULT1;
 if (operation != VR_LWS_NAMED_DISPOSE1 && v->Control->Fault)
  return v->Control->Fault & VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1 ? VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_PROTOCOL_FAULT1;
 if (operation != VR_LWS_NAMED_DISPOSE1 && b->Creating &&
     (b->Creating->Phase != VR_LWS_PHASE_STARTUP1 || b->Creating->Remaining < 512))
  return VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
 varest_ws_c2l_debit_named1(v, &r->Row, b, operation, &h);
 return vr_hook1(v, r, &h);
}

/* Pure observation never replaces LastHook or supplies a native result. */
static int
vr_observe_row1(const VrLwsCustody1 *v, const struct lws_vr_resource1 *r,
 uint32_t *state)
{
 VrLwsHookOutcome1 h = {VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
 *state = UINT32_MAX;
 varest_ws_c2l_observe_row1(v, &r->Row, state, &h);
 if (h.Disposition != VR_LWS_HOOK_COMPLETED1 || h.NativeStatus ||
     h.NativeCalls || h.Reserved || *state < VR_LWS_LOCAL_RESERVED1 ||
     *state > VR_LWS_ACQUIRE_FAULT_ACK2)
  return VR_LWS_PROTOCOL_FAULT1;
 return VR_LWS_KNOWN_SUCCESS1;
}

/* Observe the actual shared row, never stale C flags. At most TWO distinct
 * ACKs and THREE observations; a refusal or unexpected transition ends this
 * call without repeating an event. Physical/reference retirement is separate. */
int
lws_vr_ack_retire1(const VrLwsCustody1 *v, const VrLwsBudgetRef1 *b,
 struct lws_vr_resource1 *r)
{
 VrLwsHookOutcome1 h;
 uint32_t state, expected, step;
 int status;
 if (r->ObservedNative19)
  return VR_LWS_RELEASE_UNCERTAIN1;
 if (r->Stage == LWS_VR_RETIRED1 || r->Stage == LWS_VR_EMPTY1)
  return VR_LWS_KNOWN_SUCCESS1;
 if (r->Stage == LWS_VR_UNKNOWN_ACK1 || r->Stage == LWS_VR_FREE_ENTERED1)
  return VR_LWS_RELEASE_UNCERTAIN1; /* no invented full-return receipt */
 status = vr_observe_row1(v, r, &state);
 if (status != VR_LWS_KNOWN_SUCCESS1)
  return status;
 for (step = 0; step < 3; ++step) {
  /* Cross-check ACTUAL physical continuation before interpreting any ACK. */
  switch (r->Stage) {
  case LWS_VR_ACQUIRED1:
   if (state != VR_LWS_OBTAINED_PENDING1 && state != VR_LWS_OWNED_ACK1)
    return VR_LWS_PROTOCOL_FAULT1;
   break;
  case LWS_VR_NO_RESOURCE1:
   if (state != VR_LWS_NOT_OBTAINED_PENDING1 && state != VR_LWS_NOT_OBTAINED_ACK1)
    return VR_LWS_PROTOCOL_FAULT1;
   break;
  case LWS_VR_FREE_RETURNED1:
   if (state != VR_LWS_OBTAINED_THEN_CLOSED1 && state != VR_LWS_CLOSED_PENDING1 &&
       state != VR_LWS_CLOSED_ACK1)
    return VR_LWS_PROTOCOL_FAULT1;
   break;
  case LWS_VR_DISPOSE_UNKNOWN1:
   if (state != VR_LWS_OBTAINED_THEN_UNCERTAIN1 && state != VR_LWS_UNCERTAIN_PENDING1 &&
       state != VR_LWS_UNCERTAINTY_ACK1)
    return VR_LWS_PROTOCOL_FAULT1;
   break;
  case LWS_VR_UNKNOWN1:
   if (state != VR_LWS_ACQUIRE_FAULT_PENDING2 && state != VR_LWS_ACQUIRE_FAULT_ACK2)
    return VR_LWS_PROTOCOL_FAULT1;
   break;
  default:
   return VR_LWS_PROTOCOL_FAULT1;
  }
  r->AcquisitionAck = state == VR_LWS_OWNED_ACK1 || state == VR_LWS_CLOSED_PENDING1 ||
   state == VR_LWS_UNCERTAIN_PENDING1 || state == VR_LWS_UNCERTAINTY_ACK1 ||
   state == VR_LWS_CLOSED_ACK1;
  r->TerminalAck = state == VR_LWS_CLOSED_ACK1 || state == VR_LWS_NOT_OBTAINED_ACK1 ||
   state == VR_LWS_UNCERTAINTY_ACK1 || state == VR_LWS_ACQUIRE_FAULT_ACK2;
  if (state == VR_LWS_OWNED_ACK1)
   return VR_LWS_KNOWN_SUCCESS1;
  if (state == VR_LWS_UNCERTAINTY_ACK1 || state == VR_LWS_ACQUIRE_FAULT_ACK2) {
   r->Stage = LWS_VR_UNKNOWN_ACK1;
   return VR_LWS_RELEASE_UNCERTAIN1; /* physical uncertainty, not synthetic native19 */
  }
  if (state == VR_LWS_CLOSED_ACK1 || state == VR_LWS_NOT_OBTAINED_ACK1) {
   /* Genuine ALL physical/source/stack/callback references and children were
    * retired by the exact caller. Observation/terminal ACK does not prove it. */
   h = (VrLwsHookOutcome1){VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
   varest_ws_c2l_retire_source_row1(v, &r->Row, &h);
   status = vr_hook1(v, r, &h);
   if (status != VR_LWS_KNOWN_SUCCESS1)
    return status;
   r->Row = (VrLwsRowRef1){0}; r->Pointer = NULL; r->Fd = -1;
   r->Stage = LWS_VR_RETIRED1;
   return VR_LWS_KNOWN_SUCCESS1;
  }
  if (step == 2)
   return VR_LWS_PROTOCOL_FAULT1;
  switch (state) {
  case VR_LWS_OBTAINED_PENDING1: expected = VR_LWS_OWNED_ACK1; break;
  case VR_LWS_NOT_OBTAINED_PENDING1: expected = VR_LWS_NOT_OBTAINED_ACK1; break;
  case VR_LWS_OBTAINED_THEN_CLOSED1: expected = VR_LWS_CLOSED_PENDING1; break;
  case VR_LWS_CLOSED_PENDING1: expected = VR_LWS_CLOSED_ACK1; break;
  case VR_LWS_OBTAINED_THEN_UNCERTAIN1: expected = VR_LWS_UNCERTAIN_PENDING1; break;
  case VR_LWS_UNCERTAIN_PENDING1: expected = VR_LWS_UNCERTAINTY_ACK1; break;
  case VR_LWS_ACQUIRE_FAULT_PENDING2: expected = VR_LWS_ACQUIRE_FAULT_ACK2; break;
  default: return VR_LWS_PROTOCOL_FAULT1;
  }
  h = (VrLwsHookOutcome1){VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
  varest_ws_c2l_ack_next1(v, r->Row.Cell, b, &h);
  status = vr_hook1(v, r, &h);
  if (status != VR_LWS_KNOWN_SUCCESS1)
   return status;
  status = vr_observe_row1(v, r, &state);
  if (status != VR_LWS_KNOWN_SUCCESS1)
   return status;
  if (state != expected)
   return VR_LWS_PROTOCOL_FAULT1;
 }
 return VR_LWS_PROTOCOL_FAULT1;
}

int
lws_vr_never1(const VrLwsCustody1 *v, const VrLwsBudgetRef1 *b,
 struct lws_vr_resource1 *r, uint32_t where)
{
 if (r->Stage == LWS_VR_RESERVED1) {
  /* Source has not called ANY primitive for this reservation. */
  varest_ws_c2l_record_never_attempted1(v, &r->Row, where);
  r->Stage = LWS_VR_NO_RESOURCE1;
 }
 if (r->Stage != LWS_VR_NO_RESOURCE1 && r->Stage != LWS_VR_RETIRED1)
  return VR_LWS_PROTOCOL_FAULT1;
 return lws_vr_ack_retire1(v, b, r);
}

int
lws_vr_source_result1(const VrLwsCustody1 *v, VrLwsResult1 *out,
 int disposition, void *object, uint32_t where, uint32_t pending)
{
 /* Accessible separate complete output/view/code lifetime is a genuine source
  * precondition; neither these scalar checks nor this output grants it. */
 if (!out)
  return VR_LWS_PROTOCOL_FAULT1;
 if (v && v->Control && (v->Control->Fault & VR_LWS_SOURCE_FAULT_NATIVE19_OBSERVED1))
  disposition = VR_LWS_RELEASE_UNCERTAIN1;
 out->Size = sizeof(*out); out->Version = VR_LWS_CUSTODY_VERSION1;
 out->Disposition = (uint32_t)disposition; out->SourceLocation = where;
 out->RawObject = object; out->PendingRows = pending;
 out->Fault = v && v->Control ? v->Control->Fault : 0;
 return disposition;
}

/* Genuine caller/role/code/holder lifetimes precede every private operation. */
int
lws_vr_tracked1(const struct lws_context *c)
{
 return c && c->vr.View && c->vr.View->ResourceBinding;
}

int
lws_vr_data_valid1(const struct lws *wsi)
{
 const struct lws_context *c;
 unsigned int n;
 if (!wsi || !(c = wsi->context) || !lws_vr_tracked1(c) || !wsi->vr_allocation)
  return 0;
 if (wsi->event_pipe)
  return wsi->vr_allocation == &c->vr.Event && c->vr.Event.Pointer == wsi &&
   c->vr.Event.Stage == LWS_VR_ACQUIRED1;
 for (n = 0; n < 2; ++n)
  if (c->vr.Data[n].Object == wsi && wsi->vr_allocation == &c->vr.Data[n].Allocation)
   return c->vr.Data[n].Allocation.Pointer == wsi &&
    c->vr.Data[n].Allocation.Stage == LWS_VR_ACQUIRED1;
 return 0;
}

int
lws_vr_callback1(struct lws *wsi, enum lws_callback_reasons reason,
 void *user, void *in, size_t len)
{
 const VrLwsCustody1 *v;
 int result;
 if (!wsi || !wsi->protocol || !wsi->protocol->callback)
  return -1;
 if (!lws_vr_tracked1(wsi->context))
  return wsi->protocol->callback(wsi, reason, user, in, len);
 v = wsi->context->vr.View;
 if (v->Control->CallbackDepth >= 4) {
  wsi->context->vr.LastDisposition = VR_LWS_DEFERRED_BEFORE_ACQUIRE1;
  return LWS_VR_CALLBACK_DEFERRED1;
 }
 ++v->Control->CallbackDepth;
 result = wsi->protocol->callback(wsi, reason, user, in, len);
 --v->Control->CallbackDepth;
 return result; /* no borrowed callback input is retained */
}

int
lws_vr_io1(struct lws *wsi, uint32_t operation)
{
 struct lws_context *c;
 struct lws_vr_resource1 *r;
 int status;
 if (!wsi || !(c = wsi->context) || !lws_vr_data_valid1(wsi) || !c->vr.Budget ||
     !c->vr.Budget->Active || c->vr.Budget->Creating ||
     operation < VR_LWS_NAMED_CONNECT1 || operation > VR_LWS_NAMED_SHUTDOWN1)
  return VR_LWS_PROTOCOL_FAULT1;
 r = operation == VR_LWS_NAMED_POLL1 ? &c->vr.Self :
  wsi->event_pipe ? &c->vr.Wake[0] : &wsi->vr_fd;
 if (r->Stage != LWS_VR_ACQUIRED1 ||
     (operation != VR_LWS_NAMED_POLL1 && r->Fd != wsi->desc.sockfd))
  return VR_LWS_PROTOCOL_FAULT1;
 status = lws_vr_debit1(c->vr.View, c->vr.Budget, r, operation);
 if (status != VR_LWS_KNOWN_SUCCESS1) {
  c->vr.LastDisposition = status; c->vr.IoDeferred = 1;
 }
 return status;
}

void
lws_vr_io_record1(struct lws_context *c, uint32_t operation, int64_t result, int32_t error)
{
 c->vr.Io.Result = result;
 c->vr.Io.SavedError = error;
 c->vr.Io.Operation = operation; c->vr.Io.Detail = 0; c->vr.Io.DetailBytes = 0;
}

static int
vr_cache_index1(struct lws *wsi, const struct lws_buflist *node)
{
 unsigned int i;
 for (i = 0; i < 2; ++i)
  if (wsi->vr_data.Cache[i].Stage == LWS_VR_ACQUIRED1 &&
      wsi->vr_data.Cache[i].Pointer == node)
   return (int)i;
 return -1;
}

static int
vr_cache_free1(struct lws *wsi, unsigned int i)
{
 struct lws_buflist *node = wsi->vr_data.Cache[i].Pointer;
 return wsi->vr_data.Cache[i].Stage == LWS_VR_ACQUIRED1 && node &&
  node != wsi->buflist && (!wsi->buflist || node != wsi->buflist->next) &&
  !node->len && !node->pos && !node->next;
}

int
lws_vr_read_claim1(struct lws *wsi)
{
 unsigned int i;
 if (!lws_vr_data_valid1(wsi) || wsi->event_pipe || wsi->vr_data.ReadSlot != 2 ||
     wsi->vr_data.ReadBytes || wsi->vr_data.Rx.Stage != LWS_VR_ACQUIRED1)
  return 0;
 for (i = 0; i < 2; ++i)
  if (vr_cache_free1(wsi, i)) {
   wsi->vr_data.ReadSlot = i;
   return 1;
  }
 return 0; /* no input occurs without space for its entire untouched tail */
}

void
lws_vr_read_release1(struct lws *wsi)
{
 wsi->vr_data.ReadBytes = 0;
 wsi->vr_data.ReadSlot = 2;
}

size_t
lws_vr_buf_next1(struct lws *wsi, struct lws_buflist **head, uint8_t **buf)
{
 struct lws_buflist *node;
 if (buf) *buf = NULL;
 if (head != &wsi->buflist || !(node = *head))
  return 0;
 if (vr_cache_index1(wsi, node) < 0 || !node->len || node->len > 32768 ||
     node->pos >= node->len || (node->next &&
     (node->next == node || vr_cache_index1(wsi, node->next) < 0 || node->next->next))) {
  wsi->context->vr.View->Control->Fault |= VR_LWS_SOURCE_FAULT_PROTOCOL1;
  return 0;
 }
 if (buf) *buf = node->buf + node->pos;
 return node->len - node->pos;
}

int
lws_vr_buf_append1(struct lws *wsi, struct lws_buflist **head, const uint8_t *buf, size_t len)
{
 struct lws_buflist *node, *tail;
 unsigned int i;
 int first;
 if (head != &wsi->buflist || len > 32768 || (len && !buf))
  return -1;
 if (!len) {
  lws_vr_read_release1(wsi);
  return 0;
 }
 i = wsi->vr_data.ReadSlot;
 if (i >= 2 || !vr_cache_free1(wsi, i) || !wsi->vr_data.ReadBytes ||
     wsi->vr_data.ReadBytes > 32768 || len > wsi->vr_data.ReadBytes)
  return -1; /* capacity and actual fresh bytes were required BEFORE input */
 node = wsi->vr_data.Cache[i].Pointer;
 first = !*head;
 tail = *head;
 if (tail && (!lws_vr_buf_next1(wsi, head, NULL) || tail->next))
  return -1; /* at most the other prepared slab can already be queued */
 memmove(node->buf, buf, len);
 node->len = len; node->pos = 0; node->next = NULL;
 if (tail) tail->next = node; else *head = node;
 lws_vr_read_release1(wsi);
 return first;
}

int
lws_vr_buf_use1(struct lws *wsi, struct lws_buflist **head, size_t len)
{
 struct lws_buflist *node;
 size_t remaining = lws_vr_buf_next1(wsi, head, NULL);
 if (!remaining || !len || len > remaining)
  return -1;
 node = *head; node->pos += len;
 if (node->pos == node->len) {
  *head = node->next;
  node->next = NULL; node->len = node->pos = 0;
 }
 return (int)lws_vr_buf_next1(wsi, head, NULL);
}

void
lws_vr_buf_destroy1(struct lws *wsi, struct lws_buflist **head)
{
 unsigned int i;
 if (head != &wsi->buflist) {
  wsi->context->vr.View->Control->Fault |= VR_LWS_SOURCE_FAULT_PROTOCOL1;
  return;
 }
 *head = NULL;
 for (i = 0; i < 2; ++i)
  if (wsi->vr_data.Cache[i].Stage == LWS_VR_ACQUIRED1) {
   struct lws_buflist *node = wsi->vr_data.Cache[i].Pointer;
   node->next = NULL; node->len = node->pos = 0;
  }
 lws_vr_read_release1(wsi); /* logical reuse only; no physical disposal/ACK */
}

int
lws_vr_ref_ack1(const VrLwsCustody1 *v, const VrLwsBudgetRef1 *b,
 struct lws_vr_resource1 *r)
{
 int status;
 if (r->Stage == LWS_VR_EMPTY1) return VR_LWS_KNOWN_SUCCESS1;
 if (r->Stage == LWS_VR_RESERVED1) status = lws_vr_never1(v, b, r, r->Row.Cell);
 else status = lws_vr_ack_retire1(v, b, r);
 if (status == VR_LWS_KNOWN_SUCCESS1 && r->Stage == LWS_VR_RETIRED1)
  lws_vr_resource_init1(r); /* physical/source users have genuinely retired */
 return status;
}
