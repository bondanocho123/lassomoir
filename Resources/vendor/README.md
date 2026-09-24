# Vendor

Library pihak ketiga yang dibundel ke resource aplikasi (`:/vendor/...`).

| File | Sumber | Versi | Lisensi |
|------|--------|-------|---------|
| `mermaid.min.js` | https://cdn.jsdelivr.net/npm/mermaid@11.17.2/dist/mermaid.min.js | 11.17.2 | MIT (`mermaid.LICENSE`) |

`mermaid.min.js` dipakai `EdgeMermaidRenderer`: disalin ke folder sementara lalu dimuat oleh
Microsoft Edge/Chrome headless untuk menggambar blok ```mermaid``` jadi gambar. Tidak ada akses
jaringan saat render.

Saat mengganti versi, naikkan juga `kRendererVersion` di `MermaidRenderer.cpp` supaya cache
diagram lama tidak terpakai.
