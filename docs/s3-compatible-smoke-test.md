# S3-compatible storage smoke test

Holder's automated suite always checks AWS Signature Version 4 against the published AWS vector.
The provider's object round-trip test is opt-in so normal builds do not require network credentials.

## Local MinIO

Start MinIO and create a disposable bucket:

```bash
docker run --rm --name holder-minio \
  -p 9000:9000 -p 9001:9001 \
  -e MINIO_ROOT_USER=holder-test \
  -e MINIO_ROOT_PASSWORD=holder-test-secret \
  quay.io/minio/minio server /data --console-address :9001
```

In another terminal:

```bash
docker run --rm --network host minio/mc sh -c '
  mc alias set holder http://127.0.0.1:9000 holder-test holder-test-secret &&
  mc mb --ignore-existing holder/holder-assets
'

HOLDER_TEST_S3_ENDPOINT=http://127.0.0.1:9000 \
HOLDER_TEST_S3_REGION=us-east-1 \
HOLDER_TEST_S3_BUCKET=holder-assets \
HOLDER_TEST_S3_ACCESS_KEY_ID=holder-test \
HOLDER_TEST_S3_SECRET_ACCESS_KEY=holder-test-secret \
./build/tests/holder_daemon_tests '[s3][integration]'
```

The test uploads an opaque object, verifies it with `HEAD`, downloads and hashes it, removes it, and
confirms that it is gone. It also attempts cleanup if an assertion fails.

## Hosted S3-compatible service

Use the same five environment variables with an HTTPS endpoint and a disposable bucket. The test
uses path-style addressing. Credentials need permission to put, get, inspect and delete objects
beneath `holder-integration-tests/`.

Do not put credentials in Git, shell history, CI logs or a checked-in environment file. For CI,
store them as protected repository or environment secrets.
