## Summary

<!-- What changes and why. Link the issue it fixes, if any. -->

## Testing

<!-- Tick what you ran and note the results. -->

- [ ] `build/host/wmgl_host_tests` and `build/host/wmgl_gpu_tests`
- [ ] `scripts/verify-source.sh` and `scripts/check-forbidden-identifiers.sh`
- [ ] Wine fixtures (`tests/wine/probe-opengl-wow64.sh`), for changes that reach Wine
- [ ] Khronos CTS (configurations 1 to 4), for changes to rendering or GL state
- [ ] A regression test, for a bug fix

## Checklist

- [ ] No code that recognises Khronos CTS tests or shaders
- [ ] Generated docs regenerated with `scripts/docs/generate.py`, not edited by hand
- [ ] `scripts/update-checksums.sh` run after adding, removing or changing files
- [ ] `patches/wine` still applies to `wine-11.17`, if it changed
