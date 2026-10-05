# Icons

Ikon UI (selain `plus.svg` placeholder di sini) diambil dari [Flaticon](https://www.flaticon.com/).

## Cara nambah ikon baru

1. Download ikon dari Flaticon dalam format SVG (lebih tajam di semua ukuran/resolusi dibanding PNG).
2. Simpan di folder ini dengan nama singkat & deskriptif, contoh: `trash.svg`, `edit.svg`, `close.svg`.
3. Daftarkan file-nya di [`Resources/resources.qrc`](../resources.qrc) di dalam `<qresource prefix="/">`:
   ```xml
   <file>icons/trash.svg</file>
   ```
4. Pakai di kode lewat `QIcon`, contoh:
   ```cpp
   auto *btn = new QPushButton(row);
   btn->setIcon(QIcon(":/icons/trash.svg"));
   btn->setIconSize(QSize(14, 14));
   ```
   Lihat `createProjectRowWidget()` di [`mainwindow.cpp`](../../mainwindow.cpp) untuk contoh lengkap (tombol "New Task").

## Atribusi

Akun Flaticon gratis mewajibkan atribusi untuk tiap ikon yang dipakai (nama pembuat + link Flaticon).
Kalau nggak mau urus atribusi satu-satu, perlu langganan Flaticon Premium yang menghapus kewajiban itu.

Kalau tetap pakai akun gratis, catat sumber tiap ikon di sini supaya nggak lupa mau ditaruh di mana (README app / halaman About):

| File | Sumber Flaticon | Pembuat |
|------|------------------|---------|
| `plus.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `plus-white.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `sidebar.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `trash.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `trash-white.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `run.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `stop.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `review.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `expand.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `collapse.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `folders.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `image.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `paperclip.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `close.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `close-white.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `chevron-right.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `chevron-down.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `branch.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `info.svg` | dilampirkan pemilik proyek (PNG 16x16), digambar ulang sebagai SVG; sumber aslinya belum dicatat | - |
| `signal.svg` | dilampirkan pemilik proyek (PNG 16x16), digambar ulang sebagai SVG; sumber aslinya belum dicatat | - |
| `canvas.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `board.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `note.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `spark.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `undo.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
| `redo.svg` | placeholder buatan sendiri (bukan dari Flaticon) | - |
