"""Shared TLS trust store for ``urllib``-based provider requests.

Embedded interpreters do not always ship a usable CA bundle.  Binary Ninja's
bundled ``bnpython3``, for example, is a repackaged python.org build whose
OpenSSL is compiled with ``OPENSSLDIR`` pointing at
``/Library/Frameworks/Python.framework/Versions/3.10/etc/openssl`` — a path
that does not exist unless the user separately installed python.org Python.
``ssl.create_default_context()`` then loads zero CA certificates and every
HTTPS request dies with::

    [SSL: CERTIFICATE_VERIFY_FAILED] unable to get local issuer certificate

Providers that go through ``httpx`` (the OpenAI/Anthropic SDKs) are unaffected
because ``httpx`` defaults to the ``certifi`` bundle.  The ``urllib`` call sites
have no such default, so they use :func:`ssl_context` instead.

Resolution order:

1. If ``SSL_CERT_FILE`` or ``SSL_CERT_DIR`` is set, that is the operator's
   explicit trust policy — it is used as-is and never supplemented, even when
   it resolves to nothing.  Substituting a broader store behind an operator who
   pinned a narrow one would silently widen trust.
2. The interpreter's own default trust store, when it actually has CAs.
3. The ``certifi`` bundle, if importable.
4. A platform CA bundle from :data:`_FALLBACK_BUNDLES`.

Verification is never disabled.  If no bundle is found the default context is
returned unchanged so the request fails loudly rather than silently trusting
an unverified peer.
"""

from __future__ import annotations

import functools
import os
import ssl

from .logging import log_debug, log_warning

# Environment variables OpenSSL consults for an operator-chosen trust store.
_TRUST_ENV_VARS = ("SSL_CERT_FILE", "SSL_CERT_DIR")

# Platform CA bundles, checked in order when the interpreter has none of its own.
# macOS ships the first (``/etc`` is a symlink to ``/private/etc``, so listing
# both would be the same inode); the rest cover common Linux distributions.
_FALLBACK_BUNDLES = (
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
)


def _operator_trust_store_configured() -> bool:
    """Report whether the environment pins an explicit trust store.

    Returns:
        ``True`` if ``SSL_CERT_FILE`` or ``SSL_CERT_DIR`` is set to a non-empty
        value, meaning the caller has chosen a trust store deliberately.
    """
    return any(os.environ.get(name, "").strip() for name in _TRUST_ENV_VARS)


def _certifi_bundle() -> str | None:
    """Return the path to the ``certifi`` CA bundle, or ``None`` if unusable.

    Returns:
        Absolute path to ``cacert.pem``, or ``None`` when ``certifi`` is not
        installed or its bundle is missing from disk.
    """
    try:
        import certifi
    except ImportError:
        return None
    path = certifi.where()
    return path if path and os.path.exists(path) else None


def _candidate_bundles() -> tuple[str, ...]:
    """Return CA bundle paths to try, most preferred first.

    Returns:
        Existing bundle paths; may be empty if the host has no usable store.
    """
    certifi_path = _certifi_bundle()
    paths = (certifi_path, *_FALLBACK_BUNDLES) if certifi_path else _FALLBACK_BUNDLES
    return tuple(p for p in paths if os.path.exists(p))


def _has_ca_certs(context: ssl.SSLContext) -> bool:
    """Report whether a context has any trusted CA certificates loaded.

    Args:
        context: The context to inspect.

    Returns:
        ``True`` if at least one CA certificate is in the trust store.
    """
    try:
        return context.cert_store_stats().get("x509_ca", 0) > 0
    except (AttributeError, OSError):  # pragma: no cover - non-OpenSSL backends
        return False


@functools.lru_cache(maxsize=1)
def ssl_context() -> ssl.SSLContext:
    """Return a verifying :class:`ssl.SSLContext` with a working trust store.

    The result is cached for the life of the process; restart the host
    application to pick up a newly installed CA bundle.

    Returns:
        A context with hostname checking and certificate verification enabled.
    """
    context = ssl.create_default_context()
    if _operator_trust_store_configured():
        log_debug(f"TLS: using trust store pinned by {'/'.join(_TRUST_ENV_VARS)}")
        return context
    if _has_ca_certs(context):
        return context

    for path in _candidate_bundles():
        try:
            context.load_verify_locations(cafile=path)
        except (OSError, ssl.SSLError) as exc:
            log_debug(f"TLS: CA bundle {path} unusable: {exc}")
            continue
        if _has_ca_certs(context):
            log_debug(f"TLS: loaded CA bundle from {path}")
            return context

    log_warning(
        "TLS: no CA bundle found for this interpreter; HTTPS requests will fail "
        "certificate verification. Set SSL_CERT_FILE to a PEM bundle "
        "(e.g. /etc/ssl/cert.pem) or install certifi."
    )
    return context
