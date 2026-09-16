"""Use the operating system certificate store for HTTPS.

Antivirus HTTPS scanning and corporate proxies re-sign TLS traffic with their
own root, which Windows trusts but Python's bundled certifi does not - the
symptom is "CERTIFICATE_VERIFY_FAILED: self-signed certificate in certificate
chain" on the Hugging Face download. pip already avoids it via truststore; the
backend does the same. Verification stays ON - only the trust source changes.
"""

from __future__ import annotations

import logging

log = logging.getLogger("gbtts.net")
_done = False


def use_system_certificates() -> bool:
    global _done
    if _done:
        return True
    try:
        import truststore
        truststore.inject_into_ssl()
        _done = True
        return True
    except Exception as exc:  # pragma: no cover
        log.warning("truststore unavailable, using certifi bundle: %s", exc)
        return False
