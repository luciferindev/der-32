# fastfetch logo

`der32.txt` is Genesis the tortoise, the DER/32 mascot (original drawing:
`docs/art/genesis.png`), as a fastfetch ASCII logo: 9 lines × 41 columns,
plain ASCII. `$1` is the outline, `$2` the eye.

```sh
mkdir -p ~/.config/fastfetch
cp der32.txt config.jsonc ~/.config/fastfetch/
fastfetch
```

Or try it without installing:

```sh
fastfetch --logo share/fastfetch/der32.txt --logo-type file \
    --logo-color-1 white --logo-color-2 yellow
```

Built-in support (fastfetch picking the logo by itself when it detects
DER32, and counting upkg packages) needs a patch to fastfetch upstream.
