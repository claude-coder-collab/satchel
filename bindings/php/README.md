# Satchel (PHP)

PHP FFI binding for the Satchel C API. Requires PHP 8.1+ with `ffi.enable=1` (or `preload`) in the
deployment configuration.

```php
require 'src/Satchel.php';

$ctx = new Satchel\Context();
$plan = $ctx->plan(['/path/to/session']);
if (!$plan->executable()) {
    print_r($plan->conflicts());
}
$plan->build('session.zip');
$ctx->open('session.zip')->extract('restored');
```

The shared library is found through `SATCHEL_LIBRARY`, next to `src/`, or in the repository's
`build/*/core/{Release,Debug}` directories.

Tests: `php -d ffi.enable=1 tests/run.php`.
