The storage TLS certificate and private key are public test fixtures, used only by
loopback HTTP servers. They must never be used by a deployed service. The tests
trust this certificate explicitly and still verify the requested hostname.
