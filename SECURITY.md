# Security policy

FastIPC is an educational derivative project and currently has no
production-supported release line or security-response SLA.

Report suspected vulnerabilities through this repository's private GitHub
Security Advisory / private vulnerability-reporting interface when available.
If that interface is unavailable, contact the current repository owner through
a private channel before disclosing exploit details publicly.

Do not send FastIPC reports to the upstream `libsharedmemory` maintainer unless
the issue is independently reproduced in unmodified upstream code. FastIPC's
rewritten layout, transports, recovery logic, and API are maintained here.

Please include:

- affected revision and Linux/kernel details;
- transport and configuration;
- minimal reproduction;
- expected versus observed behavior;
- sanitizer or crash output;
- whether untrusted local users can reach the channel namespace.
