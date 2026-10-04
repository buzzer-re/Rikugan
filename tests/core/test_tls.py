"""Tests for rikugan.core.tls — CA bundle resolution for urllib call sites."""

from __future__ import annotations

import ssl
from pathlib import Path

import pytest

from rikugan.core import tls


def _real_bundle() -> Path | None:
    """Return an existing platform CA bundle, or None on hosts without one."""
    return next((Path(p) for p in tls._FALLBACK_BUNDLES if Path(p).exists()), None)


@pytest.fixture(autouse=True)
def _isolated_context(monkeypatch):
    """Drop the memoized context and any inherited trust-store environment."""
    for name in tls._TRUST_ENV_VARS:
        monkeypatch.delenv(name, raising=False)
    tls.ssl_context.cache_clear()
    yield
    tls.ssl_context.cache_clear()


class TestOperatorTrustStore:
    """An explicitly configured trust store is policy, not a starting point."""

    @pytest.mark.parametrize("var", tls._TRUST_ENV_VARS)
    def test_configured_store_is_never_supplemented(self, monkeypatch, var):
        """A pinned bundle resolving to nothing must NOT gain the public roots."""
        monkeypatch.setenv(var, "/nonexistent/pinned.pem")
        assert tls._operator_trust_store_configured() is True
        untouched = ssl.create_default_context().cert_store_stats()["x509_ca"]

        context = tls.ssl_context()
        assert context.cert_store_stats()["x509_ca"] == untouched
        assert context.verify_mode == ssl.CERT_REQUIRED
        assert context.check_hostname is True

    def test_unset_environment_is_not_configured(self):
        assert tls._operator_trust_store_configured() is False

    def test_whitespace_value_is_not_configured(self, monkeypatch):
        monkeypatch.setenv("SSL_CERT_FILE", "   ")
        assert tls._operator_trust_store_configured() is False


class TestCandidateBundles:
    """Bundle discovery must only ever return paths that exist."""

    def test_only_existing_paths_returned(self, monkeypatch, tmp_path):
        present = tmp_path / "present.pem"
        present.write_text("", encoding="utf-8")
        monkeypatch.setattr(tls, "_FALLBACK_BUNDLES", (str(present), str(tmp_path / "gone.pem")))

        result = tls._candidate_bundles()
        assert str(present) in result
        assert str(tmp_path / "gone.pem") not in result
        assert all(Path(p).exists() for p in result)

    def test_certifi_takes_priority(self, monkeypatch, tmp_path):
        certifi_path = tmp_path / "cacert.pem"
        fallback = tmp_path / "cert.pem"
        for p in (certifi_path, fallback):
            p.write_text("", encoding="utf-8")
        monkeypatch.setattr(tls, "_certifi_bundle", lambda: str(certifi_path))
        monkeypatch.setattr(tls, "_FALLBACK_BUNDLES", (str(fallback),))

        result = tls._candidate_bundles()
        assert result == (str(certifi_path), str(fallback))
        assert len(result) == 2

    def test_missing_certifi_is_not_fatal(self, monkeypatch):
        monkeypatch.setattr(tls, "_certifi_bundle", lambda: None)
        monkeypatch.setattr(tls, "_FALLBACK_BUNDLES", ())

        result = tls._candidate_bundles()
        assert result == ()
        assert isinstance(result, tuple)

    def test_configured_fallbacks_are_absolute_and_unique(self):
        """/etc symlinks to /private/etc on macOS; listing both is dead weight."""
        assert len(set(tls._FALLBACK_BUNDLES)) == len(tls._FALLBACK_BUNDLES)
        assert all(p.startswith("/") for p in tls._FALLBACK_BUNDLES)
        assert len(tls._FALLBACK_BUNDLES) >= 1


class TestSSLContext:
    """The returned context must always verify, bundle found or not."""

    def test_uses_interpreter_default_when_it_has_certs(self, monkeypatch):
        """A healthy interpreter should not trigger any bundle loading."""
        monkeypatch.setattr(
            tls,
            "_candidate_bundles",
            lambda: pytest.fail("bundle lookup should be skipped when defaults work"),
        )
        monkeypatch.setattr(tls, "_has_ca_certs", lambda ctx: True)

        context = tls.ssl_context()
        assert isinstance(context, ssl.SSLContext)
        assert context.verify_mode == ssl.CERT_REQUIRED
        assert context.check_hostname is True

    def test_loads_real_bundle_when_defaults_empty(self, monkeypatch, tmp_path):
        """The bnpython3 case: an empty default store repaired by a real PEM.

        Uses the real loader and the real predicate — nothing about the
        mechanism under test is mocked.
        """
        source = _real_bundle()
        if source is None:  # pragma: no cover - host without a CA bundle
            pytest.skip("no platform CA bundle available")
        bundle = tmp_path / "cert.pem"
        bundle.write_bytes(source.read_bytes())

        # A context with no CAs at all, standing in for bnpython3's broken default.
        empty = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        empty.verify_mode = ssl.CERT_REQUIRED
        assert tls._has_ca_certs(empty) is False
        monkeypatch.setattr(ssl, "create_default_context", lambda *a, **k: empty)
        monkeypatch.setattr(tls, "_candidate_bundles", lambda: (str(bundle),))

        context = tls.ssl_context()
        assert tls._has_ca_certs(context) is True
        assert context.cert_store_stats()["x509_ca"] > 0
        assert context.verify_mode == ssl.CERT_REQUIRED
        assert context.check_hostname is True

    def test_skips_unusable_bundles(self, monkeypatch, tmp_path):
        """An unreadable bundle must not stop the search."""
        bad, good = tmp_path / "bad.pem", tmp_path / "good.pem"
        bad.write_text("not a certificate", encoding="utf-8")
        source = _real_bundle()
        if source is None:  # pragma: no cover - host without a CA bundle
            pytest.skip("no platform CA bundle available")
        good.write_bytes(source.read_bytes())

        empty = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        empty.verify_mode = ssl.CERT_REQUIRED
        monkeypatch.setattr(ssl, "create_default_context", lambda *a, **k: empty)
        monkeypatch.setattr(tls, "_candidate_bundles", lambda: (str(bad), str(good)))

        context = tls.ssl_context()
        assert tls._has_ca_certs(context) is True
        assert context.verify_mode == ssl.CERT_REQUIRED
        assert context.check_hostname is True

    def test_verification_stays_on_with_no_bundle(self, monkeypatch):
        """No trust store is a loud failure, never a silent downgrade."""
        monkeypatch.setattr(tls, "_candidate_bundles", lambda: ())
        monkeypatch.setattr(tls, "_has_ca_certs", lambda ctx: False)

        context = tls.ssl_context()
        assert context.verify_mode == ssl.CERT_REQUIRED
        assert context.check_hostname is True
        assert isinstance(context, ssl.SSLContext)

    def test_result_is_memoized(self, monkeypatch):
        monkeypatch.setattr(tls, "_has_ca_certs", lambda ctx: True)

        first = tls.ssl_context()
        assert tls.ssl_context() is first
        assert tls.ssl_context.cache_info().currsize == 1
