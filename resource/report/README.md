# Report assets

The report CSS and `preview.js` are independently written for QSoC under Apache-2.0. They use no framework, icon library or copied reference code. The script handles same-document navigation and the paginated preview; its SHA-256 hash is included in the content security policy.

Droid Sans Mono and Droid Sans Fallback are unmodified fonts from the Android Open Source Project. Both use Apache-2.0. The original NOTICE and the font copyright statements are in `../license/droid-fonts.txt`. Their Latin and Chinese glyphs are embedded in generated reports.

Source: https://android.googlesource.com/platform/frameworks/base/+/android-4.4.4_r2/data/fonts/

Public mirror: https://github.com/aosp-mirror/platform_frameworks_base/tree/63ade05d76785975fc3292ca030abbaa1dda8891/data/fonts

Revision: `63ade05d76785975fc3292ca030abbaa1dda8891`

| File | SHA-256 |
|---|---|
| DroidSansMono.ttf | 12b552de765dc1265d64f9f5566649930dde4dba07da0251d9f92801e70a1047 |
| DroidSansMono.woff2 | 8b433702ed56190e73107178742439623da79cb8f53dd62a39a8f31c11d8cead |
| DroidSansFallback.ttf | 05d71b179ef97b82cf1bb91cef290c600a510f77f39b4964359e3ef88378c79d |
| DroidSansFallback.woff2 | 125957a9ba65bf995487c36b77fbf4838229ee09212b90aab0b5eda77efa4569 |

The WOFF2 files are compressed copies made with `woff2_compress <font>.ttf`, without subsetting or changing names. Keep the original fonts, compressed copies, and notice together when updating them.

The HTML embeds both WOFF2 files and the complete font notice. SVG labels use these embedded webfonts. PDF text uses the TTF files through Qt's PDF writer. Its font notice is included as a PDF attachment. Automatic shaping is disabled for technical labels so PDF extraction retains digits and signal names. Generated reports need no installed font or network access.

The complete page preview uses the same painter and page list as the PDF, including the index and parameters. Each page is stored as a separate SVG compressed by `qCompress`. The browser skips its four-byte size header and uses the native Compression Streams API to inflate one page at a time. Only the current page is mounted in the preview. Page SVGs are limited to 4 MiB each and their compressed payloads to 64 MiB in total. Exceeding these preview limits shows a notice and preserves the static HTML and PDF output.

The page reader requires JavaScript and a browser with `DecompressionStream` support. A missing capability produces a visible notice. The static circuit sheets remain readable without scripting. The reader uses the existing embedded fonts and needs no PDF plugin, external package or network connection.


## Layout runtime

`layout.js` is the unmodified elkjs 0.12.1 bundle from the official npm release. Its SHA-256 is `b1032b7cfd2a57c54c6f0e8369d7665e19a579954f7cd20a6dae28f1912ea0d7`. The wrapper commit is `624ae90aa37110033a8af59c5ccbb292708b4317`. The matching ELK commit is `3aa3b9fb98d8f9a41285b3061cff5efe79ed2ba4`.

The generator runs it through Qt Qml's JavaScript engine. Generated HTML does not include the layout runtime. Circuit symbols and the document style are independently written for QSoC.

The runtime contains components under EPL-2.0, EPL-1.0, Apache-2.0, Apache-1.1 and MIT. Original license texts, applicable notices and source headers are in `../license/elkjs.txt`. Corresponding source URLs and hashes are in `layout-sources.json` and `../license/elkjs-sources.txt`. Keep these files with the runtime when packaging or updating it. The published source versions follow the upstream release; the compiler toolchain is not claimed to reproduce the bundle byte for byte.
