/*
 * clawt-trigger-provider.h - How one forge proves a delivery is its own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The interface is called ClawtTriggerHandler and not
 * ClawtTriggerProvider because #ClawtTriggerProvider is already the enum
 * a person writes in `provider:`.  The enum names *who is calling*; this
 * names *the code that answers for them*, and one of the two had to give
 * the name up.
 *
 * Four forges disagree about authentication in ways that are not
 * cosmetic, so each gets its own implementation rather than a switch
 * inside one:
 *
 *   Forgejo, Gitea  HMAC-SHA256 over the raw body, hex, no prefix
 *   GitHub          the same digest behind a `sha256=` prefix
 *   GitLab          no digest at all -- the shared secret, verbatim
 *   generic         a bearer token
 *
 * Every implementation takes the **raw body bytes**.  A body that has
 * been through a JSON parser and back is not the body that was signed:
 * key order, whitespace and number formatting all change, and the digest
 * changes with them.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"
#include "trigger/clawt-trigger-event.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TRIGGER_HANDLER (clawt_trigger_handler_get_type())

G_DECLARE_INTERFACE(ClawtTriggerHandler, clawt_trigger_handler,
                    CLAWT, TRIGGER_HANDLER, GObject)

/**
 * ClawtTriggerHandlerInterface:
 * @parent_iface: the parent interface
 * @verify: whether the delivery proves it knows the secret
 * @event_name: which event the sender says this is
 * @delivery_id: the sender's own id for this delivery, for deduplication
 * @normalise: the delivery flattened into a #ClawtTriggerEvent
 *
 * What a forge has to answer for.
 *
 * A missing vfunc refuses and names its type. It never answers %TRUE:
 * a handler that cannot verify and says it did is an open endpoint, and
 * the whole point of naming the provider is that authentication is
 * decided somewhere specific.
 */
struct _ClawtTriggerHandlerInterface {
    GTypeInterface parent_iface;

    gboolean (*verify)(ClawtTriggerHandler  *self,
                       const gchar          *secret,
                       GHashTable           *headers,
                       const guchar         *body,
                       gsize                 body_length,
                       GError              **error);

    gchar *(*event_name)(ClawtTriggerHandler *self, GHashTable *headers);

    gchar *(*delivery_id)(ClawtTriggerHandler *self, GHashTable *headers);

    ClawtTriggerEvent *(*normalise)(ClawtTriggerHandler *self,
                                    GHashTable          *headers,
                                    const guchar        *body,
                                    gsize                body_length);
};

/**
 * clawt_trigger_handler_for:
 * @provider: which forge
 *
 * The handler for @provider.
 *
 * Returns: (transfer none): the handler, never %NULL
 */
ClawtTriggerHandler *clawt_trigger_handler_for(ClawtTriggerProvider provider);

/**
 * clawt_trigger_handler_verify:
 * @self: a #ClawtTriggerHandler
 * @secret: (nullable): the configured shared secret
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 * @body: (array length=body_length): the raw body, exactly as it arrived
 * @body_length: how many bytes of @body
 * @error: (out) (optional): return location for a #GError
 *
 * Whether this delivery proves it knows @secret.
 *
 * A %NULL or empty @secret is a refusal, not a waiver. A trigger with no
 * secret is a public endpoint that starts an agent, so "no secret means
 * no check" is the one reading that must never be available.
 *
 * Returns: %TRUE if the delivery is authentic
 */
gboolean clawt_trigger_handler_verify(ClawtTriggerHandler  *self,
                                      const gchar          *secret,
                                      GHashTable           *headers,
                                      const guchar         *body,
                                      gsize                 body_length,
                                      GError              **error);

/**
 * clawt_trigger_handler_event_name:
 * @self: a #ClawtTriggerHandler
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 *
 * Returns: (transfer full) (nullable): the event name the sender used
 */
gchar *clawt_trigger_handler_event_name(ClawtTriggerHandler *self,
                                        GHashTable          *headers);

/**
 * clawt_trigger_handler_delivery_id:
 * @self: a #ClawtTriggerHandler
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 *
 * Returns: (transfer full) (nullable): the sender's id for this delivery
 */
gchar *clawt_trigger_handler_delivery_id(ClawtTriggerHandler *self,
                                         GHashTable          *headers);

/**
 * clawt_trigger_handler_normalise:
 * @self: a #ClawtTriggerHandler
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 * @body: (array length=body_length): the raw body
 * @body_length: how many bytes of @body
 *
 * Returns: (transfer full) (nullable): the flattened event
 */
ClawtTriggerEvent *clawt_trigger_handler_normalise(ClawtTriggerHandler *self,
                                                   GHashTable          *headers,
                                                   const guchar        *body,
                                                   gsize                body_length);

/**
 * clawt_trigger_sniff_provider:
 * @headers: (element-type utf8 utf8): the request's headers, names lowercased
 * @out_provider: (out): where to put the answer
 *
 * Guesses who is calling from the headers alone.
 *
 * Only for a delivery nobody declared. Forgejo sends Gitea- and
 * GitHub-shaped headers as well as its own, so sniffing cannot be the
 * primary answer -- and it may never *widen* what a configured trigger
 * accepts, which is why the caller checks the configured provider first
 * and reaches this only when there is nothing to check against.
 *
 * The order is Forgejo, Gitea, GitHub, GitLab: most specific first, so a
 * Forgejo delivery is recognised as Forgejo rather than as the GitHub it
 * is also pretending to be.
 *
 * Returns: %TRUE if the headers name a forge this build understands
 */
gboolean clawt_trigger_sniff_provider(GHashTable           *headers,
                                      ClawtTriggerProvider *out_provider);

/**
 * clawt_trigger_provider_accepts_url_secret:
 * @provider: which provider a trigger named
 *
 * Whether a secret carried in the URL may authenticate a delivery.
 *
 * Only %CLAWT_TRIGGER_PROVIDER_GENERIC, and that is the point of the
 * generic provider: it exists for callers that are not a forge, and
 * some of them cannot set a header at all.  podomation's webhook
 * module is the one this was built for -- its `post()` takes a URL and
 * sends `application/json`, with no `Authorization` and no signature,
 * so a recipe assuming either authenticates nothing.
 *
 * A forge is refused because it *can* sign, and every one of them
 * does.  Letting a `provider: forgejo` trigger be opened by a string in
 * a query would mean the weakest scheme any caller could reach for was
 * the scheme every trigger accepted, which is exactly what "sniffing
 * may never widen a configured trigger" already forbids one layer up.
 *
 * Returns: %TRUE if a URL secret is a proof this provider takes
 */
gboolean clawt_trigger_provider_accepts_url_secret(
    ClawtTriggerProvider provider);

/**
 * clawt_trigger_verify_url_secret:
 * @provider: which provider the trigger named
 * @secret: the trigger's own secret
 * @presented: (nullable): what arrived in the URL
 * @error: (out) (optional): return location for a #GError
 *
 * Whether a capability URL opens this trigger.
 *
 * Constant-time, through clawt_secure_equals(), for the same reason the
 * header comparisons are: a `strcmp` on a secret leaks its prefix by
 * timing, and a query string is the easiest thing in the world to
 * retry.
 *
 * A URL is a weaker proof than a signature and it is worth being plain
 * about why it is still offered: it is in whatever file names the
 * endpoint, and it reaches the receiver's access log.  It is here
 * because the alternative for a sender that cannot set headers is no
 * authentication whatsoever, and an unauthenticated endpoint is worse
 * than a secret that has to be rotated.
 *
 * Returns: %TRUE if @presented proves knowledge of @secret
 */
gboolean clawt_trigger_verify_url_secret(ClawtTriggerProvider   provider,
                                         const gchar           *secret,
                                         const gchar           *presented,
                                         GError               **error);

/**
 * clawt_trigger_headers_new:
 *
 * A header table with the lifetime and comparison rules the handlers
 * expect: names already lowercased, values owned.
 *
 * Returns: (transfer full) (element-type utf8 utf8): an empty table
 */
GHashTable *clawt_trigger_headers_new(void);

/**
 * clawt_trigger_headers_add:
 * @headers: (element-type utf8 utf8): a table from clawt_trigger_headers_new()
 * @name: the header name, in any case
 * @value: (nullable): its value
 *
 * Adds one header, lowercasing the name.
 *
 * HTTP header names are case-insensitive and the four forges do not
 * agree on a spelling -- `X-GitHub-Event` and `X-Gitea-Event` differ in
 * more than the vendor -- so the case is normalised once, here, rather
 * than at each of the twelve places a handler reads one.
 */
void clawt_trigger_headers_add(GHashTable  *headers,
                               const gchar *name,
                               const gchar *value);

G_END_DECLS
