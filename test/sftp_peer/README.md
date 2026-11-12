This peer supplies real SSH and SFTP for client tests without an OS account or
system service. It binds loopback, generates keys for each run, accepts only
SFTP sessions, rejects exec requests and exits on parent stdin EOF.

Build it before running the peer or document tests:

```sh
python .github/scripts/build-test-sftp-peer.py --cache /tmp/qsoc-test-go --output build/test/qsoc_test_sftp_peer
QSOC_TEST_DEPS_REQUIRED=1 ctest --test-dir build -R 'test_qsocsftppeer|test_qsoctooldocument' --output-on-failure
```

Windows uses the `.exe` suffix. `QSOC_TEST_SFTP_PEER` can name another build.
The build script verifies the official Go 1.26.11 archive SHA-256 and disables
automatic toolchain selection. The peer is never installed with QSoC.

The supplied working directory is not a filesystem sandbox. Tests use separate
remote data and local shadow directories, and generate all fixture bytes.
The existing POSIX OpenSSH tests remain independent interoperability checks.

Dependency versions and hashes are fixed in `go.mod` and `go.sum`. `NOTICE`
retains the BSD notices for the linked dependencies and Go runtime.
