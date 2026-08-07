# MIT License
#
# Copyright (c) 2026 aufkrawall
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Source-archive fetching for the FFmpeg dependency closure.

Split out of ffmpeg_dependencies.py: that module reached the 800-line ceiling,
and download concerns (TLS trust, transient-failure handling) are a separate
unit from dependency verification and building.

Nothing here decides trust. Callers still verify every artifact by pinned SHA256
and PGP signature; this module only has to deliver the bytes, or fail loudly.
"""

from __future__ import annotations

import http.client
import os
import shutil
import socket
import ssl
import time
import urllib.error
import urllib.request
from typing import Callable, Optional

Logger = Callable[[str], None]

# The pinned closure is fetched from several hosts (github.com, ftp.gnu.org,
# downloads.xiph.org, mirror.msys2.org, gitlab.com). A release build that has
# already spent half an hour compiling should not be lost to one dropped
# connection, so transient faults are retried a bounded number of times. This is
# not a timing workaround for a race: it is the documented failure mode of
# fetching over the public internet, and every retry re-runs the same verified
# download rather than papering over a bad result.
DOWNLOAD_ATTEMPTS = 4
DOWNLOAD_BACKOFF_SECONDS = 3
DOWNLOAD_TIMEOUT_SECONDS = 180

# Faults worth another attempt: the peer hung up, the connection died mid-body,
# DNS/socket trouble. Deliberately excludes HTTPError, so a 404 (wrong pinned
# URL) or a 403 fails immediately instead of being retried four times.
TRANSIENT_ERRORS = (
    http.client.RemoteDisconnected,
    http.client.IncompleteRead,
    ConnectionError,
    socket.timeout,
    TimeoutError,
)

# Server-side faults that are worth retrying even though they arrive as HTTPError.
TRANSIENT_HTTP_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504})


def toolchain_ssl_context(msys2_dir: str) -> Optional[ssl.SSLContext]:
    """TLS trust anchored on the toolchain's CA bundle rather than the host's.

    The Actions runner's Python could not verify downloads.xiph.org ("unable to
    get local issuer certificate") while other hosts verified fine in the same
    run - its store lacked an intermediate. Verification is never disabled: with
    no bundle available we return None and the caller uses Python's default
    context, so an untrusted certificate still fails the download.
    """
    bundle = os.path.join(msys2_dir, "etc", "pki", "ca-trust", "extracted", "pem", "tls-ca-bundle.pem")
    if not os.path.exists(bundle):
        return None
    return ssl.create_default_context(cafile=bundle)


def _is_transient(error: BaseException) -> bool:
    if isinstance(error, urllib.error.HTTPError):
        return error.code in TRANSIENT_HTTP_STATUS
    if isinstance(error, urllib.error.URLError):
        return _is_transient(error.reason) if isinstance(error.reason, BaseException) else True
    return isinstance(error, TRANSIENT_ERRORS)


def download_file(
    url: str,
    destination: str,
    *,
    ssl_context: Optional[ssl.SSLContext] = None,
    logger: Optional[Logger] = None,
    attempts: int = DOWNLOAD_ATTEMPTS,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Download `url` to `destination`, retrying only transient faults.

    The body is written to a temporary file and moved into place only once it is
    complete, so an interrupted attempt can never leave a truncated archive that
    a later run would treat as cached.
    """
    log = logger or (lambda _message: None)
    temporary_path = destination + ".tmp"
    last_error: Optional[BaseException] = None
    for attempt in range(1, max(1, attempts) + 1):
        try:
            with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT_SECONDS, context=ssl_context) as response:
                with open(temporary_path, "wb") as output_file:
                    shutil.copyfileobj(response, output_file)
            os.replace(temporary_path, destination)
            return
        except Exception as error:  # noqa: BLE001 - re-raised below when not transient
            last_error = error
            if os.path.exists(temporary_path):
                os.remove(temporary_path)
            if attempt >= max(1, attempts) or not _is_transient(error):
                raise
            delay = DOWNLOAD_BACKOFF_SECONDS * attempt
            log(f"Download of {url} failed ({error}); retry {attempt + 1}/{attempts} in {delay}s")
            sleep(delay)
    if last_error is not None:  # pragma: no cover - loop either returns or raises
        raise last_error
