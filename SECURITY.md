# Security policy

## Supported versions

Security fixes go into the latest release. Older releases are not patched;
update to the latest one.

| Version | Supported |
|---|---|
| 3.3.x | Yes |
| Earlier | No |

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: on this repository's
**Security** tab, choose **Report a vulnerability**. Do not open a public
issue, pull request or discussion for a vulnerability.

Include what you can of:

- the WineMetalGL version and how it was installed (release package, MetalSharp
  or your own Wine build);
- the macOS version and Mac model;
- the steps, program or shader that triggers it, and a crash log if there is
  one;
- what an attacker could achieve.

You will get a reply within 7 days. Once the report is confirmed, a fix is
prepared privately and released, and the advisory is published with credit to
you unless you would rather stay anonymous.

## Scope

WineMetalGL runs inside Wine processes and takes input from Windows programs:
GLSL source, buffer and texture contents and sizes, and the arguments of every
GL and WGL call. In scope:

- memory-safety bugs reachable through that input (out-of-bounds reads or
  writes, use-after-free, integer overflows in sizes and offsets);
- input that crashes or hangs the host process or the GPU beyond the program
  that sent it;
- problems in the Wine module in `src/wine` or in the release packages.

Out of scope, and better reported where they belong:

- **Wine itself**: [WineHQ](https://www.winehq.org/). Wine is not a sandbox,
  and Windows programs run with the user's own privileges.
- **The MetalSharp app**: [metalsharp/MetalSharp](https://github.com/metalsharp/MetalSharp/security).
- **The vendored glslang and SPIRV-Cross**: their upstream projects. Tell us
  as well if WineMetalGL is affected.
