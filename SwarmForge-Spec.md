# SwarmForge — Spesifikasi Arsitektur & Design System

> **Versi** 0.1 (draf) · **Tanggal** 16 September 2026 · **Disusun oleh** Claude (Opus 5)
> **Basis** kode repo `LayoutDemo` — Qt 6.11.2 · MinGW 64-bit · qmake · C++17 — kondisi file yang sudah di-*stage* (belum ada commit)

Dokumen ini disusun dari kode yang benar-benar ada, bukan hanya dari rencana. Karena itu setiap bagian diberi status, dan semua perilaku Qt, flag CLI, serta angka kontras yang dikutip sudah diuji (caranya di [Lampiran C](#lampiran-c-verifikasi)).

**Status:** ✅ sudah ada di kode · 🟡 ada, tapi belum terhubung/sebagian · ⬜ rencana

**Daftar isi**

1. [Ringkasan](#1-ringkasan)
2. [Kondisi Kode Saat Ini](#2-kondisi-kode-saat-ini)
3. [Arsitektur Target](#3-arsitektur-target)
4. [Model Domain](#4-model-domain)
5. [Integrasi Agent](#5-integrasi-agent)
6. [Design System](#6-design-system)
7. [Temuan pada Kode](#7-temuan-pada-kode)
8. [Roadmap](#8-roadmap)
9. [Keputusan Terbuka](#9-keputusan-terbuka)
- [Lampiran A: Glosarium](#lampiran-a-glosarium) · [Lampiran B: Konvensi](#lampiran-b-konvensi) · [Lampiran C: Verifikasi](#lampiran-c-verifikasi)

---

## 1. Ringkasan

**SwarmForge** adalah aplikasi desktop C++/Qt Widgets untuk mengorkestrasi dan memantau agent AI pengkodean. Setiap proyek tampil sebagai *swimlane*, dan setiap task bergerak melewati 8 stage pipeline dari `WAITING` sampai `DONE`. Di setiap stage, SwarmForge menjalankan **satu proses Claude Code baru** dengan konteks sekecil mungkin. Pada stage tertentu, manusia meninjau hasilnya dulu sebelum task boleh lanjut.

### Tujuan

| # | Tujuan | Tanda tercapai |
|---|---|---|
| T1 | **Visibilitas** | Stage, state, dan log agent setiap task terlihat live tanpa membuka terminal |
| T2 | **Hemat token** | Setiap run mulai dari konteks bersih; durasi, token, dan biaya per run tampil di kartu |
| T3 | **Kendali manusia** | Task di stage ber-gate tidak bisa maju tanpa *Approve* |
| T4 | **Satu sumber kebenaran** | Hanya `TaskManager` yang mengubah data; hanya aplikasi yang menulis file sesi |
| T5 | **Lokal** | Tanpa server; seluruh state bisa dibuka ulang dari disk |

### Di luar cakupan v1

- Multi-user, kolaborasi lewat jaringan, sinkronisasi cloud.
- Fungsi IDE (mengedit kode di dalam SwarmForge).
- Agent selain Claude Code CLI.
- Beberapa run paralel untuk task yang sama.

---

## 2. Kondisi Kode Saat Ini

### 2.1 Pohon widget

```text
MainWindow                              mainwindow.ui
├── topNavBar ── labelAppName · btnGlobalNewProject · btnGlobalOpenProject (belum terhubung)
├── scrollAreaSwimlanes
│   └── SwimlaneWidget ×N               dibuat di kode, disimpan di m_swimlanes (key: projectId)
│       └── KanbanColumnWidget ×8       urutan dari PIPELINE_STAGES
│           └── KanbanCardWidget ×M     data task disimpan di dalam widget ini
└── consolePanel : ConsolePanelWidget   log + input (perintah hanya di-echo)

TaskItem + TaskManager                  sudah ditulis, belum dipakai di mana pun
```

### 2.2 Status komponen

| Komponen | File | Status | Catatan |
|---|---|---|---|
| Jendela utama | `mainwindow.*`, `main.cpp` | ✅ | Top bar, area swimlane, konsol; data awal dari `loadInitialMockData()` |
| Swimlane | `SwimlaneWidget.*` | ✅ | 8 kolom, tombol *New Task* dan *Close* |
| Kolom | `KanbanColumnWidget.*` | ✅ | Target drag & drop (ada bug posisi, lihat K2) |
| Kartu | `KanbanCardWidget.*` | ✅ | Sumber drag dengan *ghost* 75% |
| Konsol | `ConsolePanelWidget.*` | ✅ | Log + input; perintah `clear` belum berfungsi |
| Stylesheet global | `Resources/styles.qss` | ✅ | Palet krem–coklat–navy; sebagian aturan tidak berefek (§6.7) |
| Model | `TaskItem.h`, `TaskManager.*` | 🟡 | Add/move/query ada, tetapi tidak pernah diinstansiasi |
| *Open Project* | `mainwindow.ui` | 🟡 | Tombol ada, belum ada `connect` |
| Persistensi | — | ⬜ | M2 |
| Runner agent | — | ⬜ | M3 |
| Gate review | `TaskManager`, `ResponseDrawer` | 🟡 | M4: review di drawer, Setujui/Revisi/Kembalikan, serah-terima prompt antar stage; gate per proyek belum |
| Bridge MCP | — | ⬜ | M5 |

> **Penting:** saat ini **widget kartu adalah sumber kebenaran** (`KanbanCardWidget` menyimpan `m_id`, `m_title`, dst.), bukan `TaskManager`. Drag & drop juga memindahkan widget **lebih dulu**, baru memberi tahu lewat sinyal — jadi tidak ada titik untuk menolak perpindahan. Dua hal ini yang pertama diubah (M1).

### 2.3 Aliran sinyal saat ini

```text
KanbanCardWidget ──QDrag──► KanbanColumnWidget::dropEvent
                              ├─ insertCard()                     widget langsung dipindah
                              └─ cardDropped(card, stage, index)
                                   └─► SwimlaneWidget::cardMoved(projectId, …)
                                         └─► MainWindow::handleCardMoved       tulis log

SwimlaneWidget::newTaskRequested      ─► MainWindow::handleNewTaskRequested    kartu baru di WAITING
SwimlaneWidget::closeProjectRequested ─► MainWindow (lambda)                   deleteLater() swimlane
```

Konvensi nama sinyal di kode sudah tepat dan dipertahankan: **peristiwa** dalam bentuk lampau (`cardDropped`, `taskAdded`), **permintaan** berakhiran `Requested` (`newTaskRequested`).

---

## 3. Arsitektur Target

### 3.1 Prinsip

1. **Satu pemilik state.** Hanya `TaskManager` yang mengubah data task. Widget menampilkan data dan mengirim *intent*.
2. **Aliran satu arah.** *Intent* → validasi di `TaskManager` → sinyal → UI diperbarui. Intent yang ditolak tidak mengubah apa pun.
3. **Satu penulis file.** Hanya aplikasi yang menulis file sesi; proses lain meminta lewat aplikasi.
4. **Satu proses per stage.** Setiap stage dijalankan oleh proses `claude -p` baru yang berakhir ketika stage selesai.
5. **Gate ditegakkan aplikasi.** Agent hanya melaporkan hasil; `TaskManager` yang memutuskan task maju, berhenti di gate, atau gagal.

### 3.2 Diagram komponen

```text
┌───────────────────────────────── SwarmForge — satu proses Qt ─────────────────────────────────┐
│                                                                                               │
│  VIEW                 MainWindow · SwimlaneWidget · KanbanColumnWidget · KanbanCardWidget     │
│                       ConsolePanelWidget · ReviewDialog (M4)                                  │
│                            │ intent: requestMove · runStage · approve · reject    ▲ sinyal    │
│                            ▼                                                      │           │
│  DOMAIN               TaskManager ────────────────────────────────────────────────┘           │
│                            ▲                                                                  │
│                         ┌──┴────────────────────┬─────────────────────────┐                   │
│                         │                       │                         │                   │
│  INFRA            SessionStore           SwarmCoordinator           BridgeServer              │
│                (QSaveFile + JSON)     (StageSwarm/stage)         (QLocalServer, M5)           │
│                         │                       │                         ▲                   │
└─────────────────────────┼───────────────────────┼─────────────────────────┼───────────────────┘
                          ▼                       │ stdin: prompt           │ JSON Lines
                    session.json                  ▼ stdout: stream-json     │ (named pipe)
                                        ┌───────────────────┐ stdio ┌───────┴────────┐
                                        │     claude -p     │──────►│   MCP bridge   │
                                        │ (1 proses/stage)  │  MCP  │  (Python, M5)  │
                                        └───────────────────┘       └────────────────┘
```

- View mengirim *intent* ke `TaskManager` dan hanya berubah karena sinyal darinya.
- `SessionStore` memuat state saat proyek dibuka dan menyimpan setiap kali ada sinyal perubahan.
- `SwarmCoordinator` mengarahkan task ke gerombolan (`StageSwarm`) milik stage-nya; tiap run menjalankan satu proses `claude -p` lewat `AgentRuntime`, log diteruskan ke konsol (M4: juga melapor ke `TaskManager`).
- (M5) `claude` menjalankan bridge MCP sebagai proses anak lewat stdio; bridge meneruskan request ke `BridgeServer`, yang memanggil `TaskManager`.

### 3.3 Tanggung jawab kelas

| Kelas | Lapisan | Tanggung jawab | Tidak boleh |
|---|---|---|---|
| `MainWindow` | View | Merakit widget, menyambungkan sinyal, dialog proyek | Menyimpan data task |
| `SwimlaneWidget` | View | Satu proyek: header + 8 kolom; meneruskan intent beserta `projectId` | Memutuskan perpindahan valid atau tidak |
| `KanbanColumnWidget` | View | Target drop, menghitung indeks sisip | Memindahkan kartu sebelum model setuju |
| `KanbanCardWidget` | View | Menampilkan satu task, sumber drag | Menyimpan data selain `taskId` + cache tampilan |
| `ConsolePanelWidget` | View | Log read-only + input perintah | Menjalankan perintah sendiri |
| `ResponseDrawer` ✅ | View | Drawer hasil agent (pengganti ReviewDialog): pilih run / Live, metrik, dokumen, keputusan Setujui · Revisi · Kembalikan | Mengubah state sendiri (kirim intent ke `TaskManager`) |
| `MarkdownView` ✅ | View | Markdown dialek GitHub + blok ```mermaid``` sebagai gambar lewat `MermaidRenderer`; klik diagram membuka `DiagramViewer` | Tahu cara menggambar diagram |
| `DiagramViewer` ✅ | View | Jendela non-modal satu diagram: zoom (Ctrl + scroll/pinch di titik kursor, −/+, Paskan, 100%), geser dengan seret, klik ganda Paskan ↔ 100% | Merender diagram sendiri |
| `MermaidRenderer` / `EdgeMermaidRenderer` ✅ | Infra | Kode Mermaid → PNG: Edge/Chrome headless + `mermaid.min.js` bundel (`:/vendor`); diagram yang lebih besar dari kanvas 1400 × 2400 dirender ulang seukuran aslinya (tajam saat di-zoom); cache per diagram beserta kerapatan pikselnya | — |
| `TaskManager` | Domain | State task, validasi transisi, sinyal perubahan | Menyentuh file atau proses |
| `SessionStore` ⬜ | Infra | Load/save JSON secara atomik, cek `schemaVersion` | Dipanggil langsung dari widget |
| `StageProfile` ✅ | Domain | Satu stage = key + `AgentDefinition` opsional + `TransitionPolicy` | Menjalankan agent |
| `StageCatalog` ✅ | Domain | Urutan & nilai awal 8 stage; satu-satunya sumber key stage | Diubah saat runtime |
| `TransitionPolicy` ✅ | Domain | Gate keluar stage (`ApprovalGate`, `AutoAdvance`) | Tahu soal agent |
| `SwarmCoordinator` ✅ | Infra | Prasyarat run, routing task ke `StageSwarm` stage-nya, cancel per project | Mengubah state tanpa `TaskManager` |
| `StageSwarm` ✅ | Infra | Gerombolan satu stage: `maxConcurrent` run paralel, antrean FIFO | Tahu soal `QProcess` |
| `WorkspaceGuard` ✅ | Infra | Paling banyak satu agent penulis per folder kerja | — |
| `AgentRuntime` / `AgentSession` ✅ | Infra | Kontrak menjalankan satu agent; implementasi `ClaudeCodeRuntime`/`ClaudeCodeSession` (`QProcess`, stdin, timeout, cancel) | Parsing stream-json sendiri |
| `ClaudeCli` · `StreamJsonParser` ✅ | Infra | Flag & lokasi CLI · format stream-json | Menyimpan state |
| `RunLogFormatter` ✅ | View | Teks baris konsol untuk kejadian run | — |
| `SidePanelDock` · `VerticalTabButton` ✅ | View | Tiga keadaan panel Lieutenant di pane kanan `mainSplitter`: terpasang (pin), tab tegak di rel tepi kanan, atau disembunyikan (lihat §6.6 Konsol) | Tahu isi panelnya |
| `CanvasWorkspace` ✅ | View | Satu kanvas brainstorm per project (§3.6): muat/simpan `canvas.json` lewat `FileManager`, segarkan kartu referensi saat task berubah, rakit `AgentLaunch` langkah AI, buat task usulan lewat `TaskManager` | Mengubah task tanpa `TaskManager` |
| `CanvasPage` · `CanvasInspector` · `CanvasLibrary` ✅ | View | Halaman kanvas: toolbar, tombol mengambang di kanvas (buat kartu, zoom), Pustaka artefak lintas project (sumber drag), panel Detail kartu terpilih | Menyimpan data kanvas sendiri |
| `CanvasPreviewDrawer` ✅ | View | Drawer pratinjau Markdown kartu terpilih, menimpa kanvas dari tepi kanannya (§3.6) | Tahu kartu atau model kanvas (isi datang dari `CanvasPage`) |
| `CanvasChatPanel` ✅ | View | Panel chat tanya jawab (bergantian dengan Detail): gelembung pesan, jawaban Markdown, keluaran live, bahan = kartu terpilih | Menjalankan agent sendiri (kirim intent) |
| `CanvasChat` ✅ | Infra | Percakapan satu kanvas: pesan + metrik, satu pertanyaan pada satu waktu lewat `AgentRuntime`, JSON untuk `canvas.json` | Mengubah kartu kanvas |
| `CanvasView` · `CanvasNodeItem` · `CanvasEdgeItem` ✅ | View | Kanvas tak terbatas (`QGraphicsView`): geser, zoom di kursor, seleksi, sambungan titik → kartu; semua perubahan dikirim sebagai intent ke `CanvasModel` | Mengubah kartu tanpa model |
| `CanvasModel` · `CanvasBoard` ✅ | Domain | Satu-satunya pengubah isi kanvas: validasi intent (garis tanpa putaran), undo/redo, JSON, urutan dependensi langkah AI | Menyentuh file atau proses |
| `CanvasWorkflow` ✅ | Domain | Isi kartu referensi dari task, prompt langkah AI dan chat, ringkasan task baru, parsing usulan task | UI, proses |
| `CanvasAutomation` ✅ | Infra | Antrean langkah AI urut dependensi (maks. 2 paralel), hasil ke `CanvasModel` | Tahu soal `QProcess` |
| `BridgeServer` ⬜ | Infra | `QLocalServer` untuk bridge MCP, validasi *run key* | Menulis file sesi |

`TaskManager` dibuat di `main.cpp` lalu diteruskan ke `MainWindow` (mis. `MainWindow(TaskManager *tasks, QWidget *parent = nullptr)`), supaya logika domain bisa dites tanpa membuka jendela.

### 3.4 Alur: memindahkan kartu (M1)

1. `KanbanCardWidget` hanya memulai drag bila state task `Idle` atau `Failed`. Data MIME `application/x-swarmforge-task` berisi **taskId**.
2. `KanbanColumnWidget::dropEvent` mengambil kartu dari `event->source()`, menghitung indeks **di antara kartu lain** (kartu yang di-drag tidak dihitung), lalu `emit moveRequested(taskId, stage, index)`.
3. `SwimlaneWidget` menambahkan `projectId` dan meneruskannya ke `TaskManager::requestMove(…)`.
4. Ditolak (beda proyek, state tidak mengizinkan) → log `TASK  ditolak: <alasan>`; tidak ada yang bergerak.
5. Diterima → sinyal `taskMoved(taskId, from, to, index)` → swimlane memindahkan widget → `SessionStore` menyimpan (debounce).
6. Setelah `drag->exec()` kembali, kartu **selalu** memanggil `show()`: posisinya sudah final, dipindah atau tidak.

### 3.5 Alur: menjalankan satu stage (M3–M4)

1. Pengguna menekan **Run** pada kartu `Idle` (stage selain `WAITING`/`DONE`) → `TaskManager::startRun()` → `Running`.
2. `StageRunner` membuat folder run `runs/<taskId>/<STAGE>-<n>/` (berisi `prompt.md`; di M5 juga `mcp.json`).
3. `QProcess` menjalankan `claude` di `workingDirectory` proyek; prompt ditulis ke **stdin**, lalu stdin ditutup.
4. Setiap baris stdout adalah satu event JSON: disalin ke `events.jsonl` dan diringkas ke konsol dengan sumber `AGENT:<STAGE>`.
5. Event `result` → metrik disimpan ke `StageRun`; bila `subtype` = `success`, `structured_output` dibaca sebagai hasil stage.
6. `TaskManager::finishRun()`: stage ber-gate **atau** `status` = `"blocked"` → `AwaitingReview`; selain itu → `Idle` di stage berikutnya.
7. Tanpa event `result`, exit code ≠ 0, timeout, atau `subtype` selain `success` → `failRun()` → `Failed` beserta alasannya.

### 3.6 Kanvas brainstorm ✅

Ruang brainstorm pribadi per project berupa kanvas tak terbatas. Isinya ide sendiri dan artefak dari banyak task, juga dari project lain yang sedang terbuka. Langkah AI di kanvas mengolah bahan yang disambungkan ke sana menjadi dokumen atau task baru untuk pipeline.

Dibuka lewat tombol **Kanvas** di header swimlane atau **View › Kanvas Brainstorm** (`Ctrl+Shift+K`). Tombol **Board** kembali ke kanban. Tata letaknya: toolbar di atas, **Pustaka artefak** di kiri, kanvas di tengah, **Detail** kartu terpilih di kanan. Tombol yang bekerja pada kanvas mengambang di atasnya: dua tombol bulat **Catatan** dan **Langkah AI** di pojok kiri atas, zoom (− / persen / +) dan **Paskan** di pojok kiri bawah. Toolbar tinggal memuat Board, judul (terpotong "…" bila sempit), ▶ / ■, undo/redo, pesan status, dan tombol panel, sehingga halaman kanvas tetap muat di antara sidebar dan panel Lieutenant yang terpasang.

| Kartu | Isi | Asal |
|---|---|---|
| Catatan | Teks bebas; 5 warna kertas | Klik dua kali di ruang kosong, `N`, tombol bulat **Catatan** |
| Artefak | Dokumen hasil satu stage (run sukses terakhir) atau satu lampiran task; foto tampil sebagai gambar | Seret dari Pustaka, atau klik dua kali di sana |
| Task | Judul, stage, dan status task | Seret baris task dari Pustaka; task usulan |
| Langkah AI | Instruksi + model/effort; keluaran **dokumen Markdown** atau **task untuk pipeline** | `L`, tombol bulat **Langkah AI** (ditahan atau klik kanan: pilih jenis keluaran) |

Aturan:

1. **Sambungan berarah.** Tarik dari salah satu titik ● di tengah sisi kartu (atau `Alt` + seret dari badan kartu) ke kartu lain. Dilepas di ruang kosong, sambungan membuat catatan baru yang titik tempelnya tepat di titik lepas. Garis yang masuk ke langkah AI adalah bahan agent. Garis yang membuat putaran ditolak (`CanvasModel::connectNodes`), jadi langkah AI selalu bisa diurutkan.
   Garis menempel di dua sisi kartu yang saling berhadapan menurut letak tujuannya (`canvasRoute`): mendatar bila tujuan di kiri/kanan, tegak bila di atas/bawah. Tujuan yang letaknya serong punya dua pilihan lorong; dipakai lorong yang tidak diisi kartu lain, dan bila sama saja, lorong yang garisnya lebih lurus (seri: mendatar). Karena itu garis ke kartu-kartu yang berjajar dalam satu kolom tetap masuk dari samping, tidak terjepit di sela kartu. Sisi tempel tidak disimpan di `canvas.json`: semua garis ditata ulang setiap kali ada kartu yang berpindah, bertambah, atau hilang.
2. **Referensi hidup.** Kartu artefak dan task disegarkan setiap kali task sumbernya berubah. Bila sumbernya hilang (task dihapus, project ditutup), salinan terakhir tetap dipakai dan kartunya ditandai *tidak tersedia*.
3. **Menjalankan.** **▶ Jalankan** menjalankan satu langkah, **Jalankan + hulunya** menyertakan langkah hulu, dan ikon **▶** hijau di toolbar (*Jalankan alur*) menjalankan semua langkah; ikon **■** merah di sebelahnya menghentikan semua langkah yang antre atau berjalan. Urutannya mengikuti dependensi, dengan paling banyak 2 agent paralel. Hasil langkah hulu menjadi bahan langkah hilir. Hulu yang gagal membuat hilirnya dilewati (`skipped`), bukan dijalankan dengan bahan kurang.
4. **Agent baca-saja.** `claude -p` berjalan di folder kerja project dengan tool `Read`, `Grep`, `Glob` saja dan prompt peran `:/prompts/BRAINSTORM.md`. Bawaannya sonnet/medium. Bahan dibatasi 24 000 karakter per kartu dan 90 000 per langkah. Foto lampiran ikut sebagai blok gambar.
5. **Hasil ke pipeline.** Hasil langkah bisa disalin menjadi catatan. **Jadikan task…** membuka form *New Task* yang sudah berisi judul dan ringkasan bahan yang tersambung. Langkah berkeluaran *task* menjawab dengan blok ` ```json ` berisi `[{title, category, instructions}]` (maks. 12). **Buat N task di WAITING** menambahkannya lewat `TaskManager`, lalu kartu task-nya muncul di kanvas, tersambung ke langkahnya.
6. **Undo/redo** (`Ctrl+Z` / `Ctrl+Y`, 200 langkah) mencakup kartu, garis, posisi, dan isi. Hasil langkah AI dan salinan isi referensi tidak ikut di-undo, supaya run yang sudah dibayar tidak hilang.
7. **Penempatan.** Kartu yang posisinya tidak dipilih pengguna (dari Pustaka lewat klik dua kali, task usulan, hasil yang dijadikan catatan) ditaruh di tempat lapang terdekat (`CanvasBoard::openSpot`), tidak menimpa kartu lain.
8. **Zoom.** Isi kartu ditata menurut ukurannya di layar (`CanvasNodeItem`), supaya papan besar tetap terbaca sebagai peta dan isi kartu selalu pas dengan kartunya. Sampai ±83% kartu sekadar diperbesar atau diperkecil. Di bawah itu huruf berhenti mengecil di batas yang masih terbaca (di layar: judul 10,5 px, isi 9,5 px, keterangan 9 px) dan kartu menampilkan sebanyak yang muat: judul lebih dulu, lalu keterangan, lalu isi. Yang tidak muat dipotong di batas baris dengan elipsis, tidak pernah di tengah huruf.
   Kartu referensi dan langkah AI berpita warna jenisnya di puncak kartu; pita itu memuat nama jenis dan status selama masih ada tempat untuk dua baris judul, selain itu tinggal garis warna. Judul yang tetap tidak muat memakai huruf kecil (8 px di layar) sebelum dipotong. Catatan di bawah 72% dilukis sebagai ringkasan: baris pertamanya (bila tidak lebih dari 90 karakter) menjadi judul tebal dan sisanya pratinjau tanpa tanda Markdown; pada zoom biasa teksnya apa adanya, sama seperti di editornya.
   Titik sambung, pegangan ubah ukuran, mata panah, dan daerah klik garis berukuran px layar, jadi tetap terlihat dan bisa dikenai saat diperkecil; pada kartu yang kecil di layar daerahnya ikut mengecil supaya kartunya masih bisa digeser.
9. **Pratinjau Markdown.** Tombol **Pratinjau** di kepala panel Detail, atau klik kanan catatan › **Pratinjau Markdown**, membuka drawer (`CanvasPreviewDrawer`) berisi isi kartu terpilih dalam bentuk jadi: catatan (mengikuti ketikan di editor, juga yang belum tersimpan), dokumen artefak, atau hasil langkah AI. Drawer meluncur dari tepi kanan kanvas dan menimpanya tanpa menggeser tata letak; lebarnya 55% ruang di kiri panel kanan (420–720 px), atau seluruh ruang itu bila sempit, sehingga Pustaka ikut tertutup. Tombol perluas menutupi seluruh halaman di bawah toolbar; ✕, `Esc` di dalam drawer, atau tombol Pratinjau menutupnya.

#### Chat tanya jawab

Panel kanan bergantian antara **Detail** dan **Chat** (tombol di toolbar; tombol yang aktif menutup panel). Chat dibuka juga dengan `C` atau menu klik kanan **Tanyakan kartu ini di chat**.

1. **Bahan mengikuti pilihan.** Kartu yang sedang dipilih menjadi bahan pertanyaan berikutnya; tanpa pilihan, bahannya seluruh kanvas. Baris **Bahan** di atas input menunjukkannya. Seluruh kanvas membagi jatah 90 000 karakter rata per kartu (min. 6 000, maks. 24 000).
2. **Satu proses per pertanyaan.** Seperti langkah AI, setiap pertanyaan menjalankan `claude -p` baru yang baca-saja (`Read`, `Grep`, `Glob`) di folder kerja project, dengan prompt peran `:/prompts/BRAINSTORM_CHAT.md`. Prompt-nya dirakit ulang: bahan kartu, percakapan sebelumnya (maks. 16 pesan / 20 000 karakter, pesan terbaru didahulukan, jawaban gagal tidak ikut), lalu pertanyaannya. Tidak memakai `--resume`, jadi pertanyaan tidak bergantung pada sesi lama dan bahan selalu yang terbaru.
3. **Satu pertanyaan pada satu waktu.** Selama agent menjawab, tombol **Kirim** menjadi **■ Hentikan**. Teks agent dan tool yang sedang dipakai tampil live. Jawaban yang dihentikan menyimpan potongan yang sudah masuk; jawaban gagal menampilkan alasannya beserta **Tanya lagi**.
4. **Ke kanvas.** **Jadikan catatan** menaruh pertanyaan (sebagai baris pertama) dan jawabannya di catatan biru di kanan kartu bahannya, tersambung dari kartu-kartu itu (bila ≤ 6), di tempat lapang. Langkah ini masuk undo seperti intent lain; percakapannya sendiri tidak.
5. **Tersimpan.** Percakapan (maks. 200 pesan) ikut `canvas.json` (§4.6). **Mulai ulang** menghapusnya setelah konfirmasi. Klik bahan di gelembung pertanyaan memilih kartu-kartunya lagi di kanvas.

| Pintasan | Aksi |
|---|---|
| Klik dua kali di ruang kosong · `N` | Catatan baru |
| `L` | Langkah AI baru |
| `Enter` / `F2` | Edit catatan; kartu lain: buka di Detail |
| `Ctrl+Enter` · `Ctrl+Shift+Enter` | Jalankan langkah AI terpilih · beserta hulunya |
| `Del` · `Ctrl+D` · `Ctrl+A` | Hapus · duplikat · pilih semua |
| Panah (`Shift` = 50 px) | Geser kartu terpilih |
| `Spasi` + seret · tombol tengah | Geser kanvas |
| `Ctrl` + scroll · `+` / `−` · `Ctrl+0` · `Ctrl+1` | Zoom di kursor · zoom · tampilkan semua · 100% |
| `C` | Buka chat dengan kartu terpilih (atau seluruh kanvas) sebagai bahan |
| `Esc` | Batalkan sambungan / kosongkan pilihan; di editor catatan: simpan |
| `Enter` · `Shift+Enter` (input chat) | Kirim pertanyaan · baris baru |

---

## 4. Model Domain

### 4.1 Stage

| # | Key | Peran (usulan) | Artefak utama |
|---|---|---|---|
| 0 | `WAITING` | Antrean, tanpa agent | — |
| 1 | `SPECIFIER` | Mengubah permintaan menjadi spesifikasi + kriteria penerimaan | `spec.md` |
| 2 | `CODER` | Implementasi sesuai spesifikasi | Perubahan kode |
| 3 | `CLEANER` | Refaktor: CRAP, DRY, coverage, test | Perubahan kode + metrik |
| 4 | `ARCHITECT` | Meninjau struktur dan dependensi | `architecture-notes.md` |
| 5 | `HARDENER` | Kasus tepi, penanganan error, keamanan | Test tambahan |
| 6 | `QA` | Menjalankan test, memeriksa kriteria penerimaan | `qa-report.md` |
| 7 | `DONE` | Selesai | — |

Aturan:

- Key UPPERCASE di atas dipakai di JSON dan label UI; di C++ dipakai `enum class Stage` (§4.3).
- Hasil agent hanya memajukan task **satu** stage. Drag manual boleh ke stage mana pun **di swimlane yang sama** — dianggap keputusan manusia.
- Pindah antar swimlane ditolak.

### 4.2 State di dalam stage

> **Implementasi (M4-lite):** `TaskState` = `Idle` · `AwaitingReview` · `Failed` disimpan di `session.json`; `Queued`/`Running` adalah `RunState` runtime dari `SwarmCoordinator`. `TaskManager::recordRun` menerapkan tabel di bawah; keputusan review (`approve` · `requestRevision` · `sendBack`) tercatat di `StageRun.decision` + `reviewNote`, dan `TaskPromptComposer` meneruskan dokumen yang disetujui serta hasil stage sebelumnya ke prompt stage berikutnya. Drag manual: mundur selalu boleh, maju melewati gate hanya bila gate itu sudah disetujui.

```text
                    (task masuk ke stage)
                              │
                              ▼
                      ┌───────────────┐
           ┌─────────►│     Idle      │
           │          └───────┬───────┘
           │                  │
 cancel()  │                  │ runStage()
           │                  ▼          exit≠0 / timeout / budget habis
           │          ┌───────────────┐                       ┌─────────────┐
           └──────────┤    Running    ├──────────────────────►│   Failed    │
                      └───────┬───────┘                       └──────┬──────┘
                              │   ▲                                  │
                              │   └────────── retry() ───────────────┘
                              │ result sukses
              ┌───────────────┴───────────────┐
              │ tanpa gate                    │ ber-gate / status "blocked"
              ▼                               ▼
       Idle @ stage+1             ┌───────────────────────┐
                                  │    AwaitingReview     │
                                  └───┬───────────────┬───┘
                              approve │               │ reject + catatan
                                      ▼               ▼
                               Idle @ stage+1   Idle @ stage yang sama
```

| Dari | Pemicu | Ke |
|---|---|---|
| `Idle` | Run | `Running` |
| `Running` | `result` sukses, stage tanpa gate | `Idle` @ stage+1 |
| `Running` | `result` sukses, stage ber-gate atau `status: "blocked"` | `AwaitingReview` |
| `Running` | exit ≠ 0 · timeout · budget habis · `subtype` error | `Failed` |
| `Running` | Cancel | `Idle` (stage sama) |
| `AwaitingReview` | Approve | `Idle` @ stage+1 |
| `AwaitingReview` | Reject + catatan | `Idle` (stage sama); catatan masuk ke prompt run berikutnya |
| `Failed` | Retry | `Running` |
| `Idle`, `Failed` | Drag manual | `Idle` @ stage tujuan |

`Running` dan `AwaitingReview` tidak bisa di-drag. Saat file sesi dimuat, state `Running` diubah menjadi `Failed` dengan alasan "aplikasi ditutup saat run berjalan".

### 4.3 Struktur data

```cpp
// Stage.h
#pragma once
#include <QString>
#include <array>
#include <optional>

enum class Stage { Waiting, Specifier, Coder, Cleaner, Architect, Hardener, QA, Done };
enum class TaskState { Idle, Running, AwaitingReview, Failed };

inline constexpr std::array<Stage, 8> kPipeline = {
    Stage::Waiting, Stage::Specifier, Stage::Coder, Stage::Cleaner,
    Stage::Architect, Stage::Hardener, Stage::QA, Stage::Done,
};

QString stageKey(Stage stage);                          // Stage::Coder -> "CODER"
std::optional<Stage> stageFromKey(const QString &key);  // "CODER" -> Stage::Coder; key lain -> nullopt
std::optional<Stage> nextStage(Stage stage);            // Stage::Done -> nullopt
```

```cpp
// TaskItem.h (v2)
struct StageRun {
    Stage       stage = Stage::Waiting;
    QDateTime   startedAt;
    qint64      durationMs = 0;
    qint64      inputTokens = 0;       // input_tokens + cache_creation_input_tokens
    qint64      cacheReadTokens = 0;   // cache_read_input_tokens
    qint64      outputTokens = 0;
    double      costUsd = 0.0;
    QString     sessionId;             // untuk `claude --resume <id>` saat debugging
    QString     outcome;               // "success", subtype error, "timeout", "cancelled"
    QString     summary;               // dari structured_output
    QStringList artifacts;
};

struct TaskItem {
    QString   id;                      // QUuid tanpa kurung kurawal
    QString   projectId;
    QString   title;
    QString   description;
    QString   category;                // "component", "utility", …
    Stage     stage = Stage::Waiting;
    TaskState state = TaskState::Idle;
    int       order = 0;               // posisi di dalam kolom
    int       approvals = 0;           // kandidat isi badge "✓ N" (lihat §9)
    QString   reviewNote;              // catatan Reject terakhir
    QList<StageRun> runs;
};
```

| `TaskItem` sekarang | Masalah | Di v2 |
|---|---|---|
| `QString stage` | Salah ketik tidak terdeteksi | `Stage stage` |
| `QString subtext` (`"12s 53ms \| 107k"`) | Angka disimpan sebagai teks | Dihitung dari `runs.last()` saat render |
| `QString badge` (`"✓ 1"`) | Maknanya belum jelas | Dihitung dari field terstruktur (§9) |
| — | Urutan kartu hilang | `int order` |

### 4.4 API `TaskManager` (v2)

```cpp
class TaskManager : public QObject {
    Q_OBJECT
public:
    // Query
    QList<TaskItem> tasksForProject(const QString &projectId) const;   // urut (stage, order)
    std::optional<TaskItem> task(const QString &taskId) const;

    // Intent — mengembalikan false dan mengisi *reason bila ditolak
    QString createTask(const QString &projectId, const QString &title, const QString &category);
    bool requestMove(const QString &taskId, const QString &projectId, Stage to, int index,
                     QString *reason = nullptr);
    bool startRun(const QString &taskId, QString *reason = nullptr);   // dari Idle atau Failed (retry)
    bool cancelRun(const QString &taskId, QString *reason = nullptr);
    bool approve(const QString &taskId, QString *reason = nullptr);
    bool reject(const QString &taskId, const QString &note, QString *reason = nullptr);

    // Dipanggil StageRunner; diabaikan bila task sudah tidak Running
    void finishRun(const QString &taskId, const StageRun &run, const QJsonObject &result);
    void failRun(const QString &taskId, const StageRun &run, const QString &error);

signals:
    void taskAdded(const TaskItem &task);
    void taskMoved(const QString &taskId, Stage from, Stage to, int index);
    void taskUpdated(const TaskItem &task);   // state, runs, catatan
    void taskRemoved(const QString &taskId);
};
```

### 4.5 File sesi

Lokasi: `QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)` → `%APPDATA%\SwarmForge\` (panggil `QCoreApplication::setApplicationName("SwarmForge")` di `main.cpp`; tanpa itu nama folder mengikuti nama executable). Repo target tetap bersih.

```text
SwarmForge/
└── projects/
    └── spacewar/
        ├── session.json             satu-satunya file state, hanya ditulis aplikasi
        ├── canvas.json              kanvas brainstorm project (§4.6)
        ├── artifacts/<taskId>/      spec.md, qa-report.md, …
        └── runs/<taskId>/CODER-2/
            ├── prompt.md
            ├── events.jsonl         salinan stdout stream-json (audit & debugging)
            └── mcp.json             M5; dihapus setelah run selesai
```

```json
{
  "schemaVersion": 1,
  "project": {
    "id": "spacewar",
    "title": "spacewar",
    "workingDirectory": "E:/Projects/spacewar",
    "gates": ["SPECIFIER", "QA"]
  },
  "tasks": [
    {
      "id": "3f6c1e0a-6a0e-4a8e-9a53-2f1f0c9d7b21",
      "title": "epoch-distance-memo",
      "description": "Memoize perhitungan jarak antar-epoch.",
      "category": "utility",
      "stage": "CODER",
      "state": "Idle",
      "order": 0,
      "approvals": 1,
      "reviewNote": "",
      "runs": [
        {
          "stage": "SPECIFIER",
          "startedAt": "2026-09-16T03:02:11Z",
          "durationMs": 154000,
          "inputTokens": 13200,
          "cacheReadTokens": 28000,
          "outputTokens": 6100,
          "costUsd": 0.42,
          "sessionId": "b1c9e7d2-…",
          "outcome": "success",
          "summary": "Spesifikasi dan 5 kriteria penerimaan ditulis.",
          "artifacts": ["spec.md"]
        }
      ]
    }
  ]
}
```

Aturan:

1. Ditulis dengan `QSaveFile`: isi ditulis ke file sementara di folder yang sama, lalu di-*rename* secara atomik saat `commit()`.
2. Simpan dengan debounce ±500 ms setelah perubahan terakhir, dan sekali lagi saat aplikasi ditutup.
3. `schemaVersion` tidak dikenal → tolak dibuka dengan pesan jelas; **jangan** menimpa file.
4. Stage tidak dikenal → task dipindah ke `WAITING` + log `ERROR`.
5. State `Running` saat dimuat → `Failed` (lihat §4.2).
6. Waktu dalam ISO-8601 UTC.

### 4.6 File kanvas

`projects/<projectId>/canvas.json` dirakit `CanvasBoard::toJson()` (ditambah `CanvasChat::toJson()` untuk percakapan chat) dan ditulis `FileManager` dengan aturan yang sama seperti `session.json` (`QSaveFile`, debounce ±500 ms, sekali lagi saat aplikasi ditutup). File ini ikut terhapus bersama project.

```json
{
  "schemaVersion": 1,
  "view": { "x": 700, "y": 200, "zoom": 0.75 },
  "nodes": [
    { "id": "4f0c…", "kind": "note", "x": 380, "y": -20, "width": 220, "height": 150,
      "text": "Ide: login tanpa password", "color": "amber" },
    { "id": "9b2e…", "kind": "artifact", "x": 0, "y": 0, "width": 280, "height": 180,
      "source": { "projectId": "TTT", "taskId": "1001", "stage": "SPECIFIER" },
      "title": "Spesifikasi · Login magic link", "detail": "SPECIFIER · disetujui", "text": "# Spesifikasi …" },
    { "id": "c71d…", "kind": "step", "x": 760, "y": 60, "width": 300, "height": 200,
      "text": "Gabungkan bahan ini jadi rencana rilis", "model": "opus", "output": "document",
      "run": { "success": true, "outcome": "success", "message": "# Rencana rilis …", "sessionId": "…",
               "durationMs": 38000, "totalTokens": 21000, "costUsd": 0.08, "finishedAt": "2026-10-05T12:00:00Z" } }
  ],
  "edges": [ { "id": "e5a3…", "from": "9b2e…", "to": "c71d…" } ],
  "chat": [
    { "id": "1f7a…", "role": "user", "text": "Apa risiko terbesarnya?", "at": "2026-10-05T12:03:00Z",
      "context": ["9b2e…"], "contextLabel": "1 kartu: Spesifikasi · Login magic link" },
    { "id": "8d2c…", "role": "agent", "text": "Risiko terbesar: email terlambat …", "at": "2026-10-05T12:03:21Z",
      "replyTo": "1f7a…", "model": "sonnet", "outcome": "success", "durationMs": 21000, "totalTokens": 18400, "costUsd": 0.06 }
  ]
}
```

Aturan:

1. `id` kartu dan garis berupa UUID. `kind`: `note` · `artifact` (`source.stage` = dokumen stage, atau `source.attachment` = nama lampiran) · `task` · `step`. `model`/`effort` kosong = bawaan langkah AI; `output`: `document` | `tasks`.
2. Kartu referensi menyimpan salinan isi sumbernya (maks. 60 000 karakter), supaya kanvas tetap bermakna setelah task sumbernya hilang. Status *tersedia* tidak disimpan; dihitung ulang saat dimuat.
3. `schemaVersion` tidak dikenal atau JSON rusak → file dipindah ke `canvas-rusak-<yyyyMMdd-HHmmss>.json` (isinya tidak dibuang), kanvas mulai kosong, dan konsol mencatat `[KANVAS WARN]`.
4. Kartu berjenis tidak dikenal, id ganda, dan garis yang menunjuk kartu yang tidak ada atau membuat putaran dilewati saat dimuat, masing-masing dengan peringatan.
5. Hanya hasil run yang sudah selesai yang disimpan (`run`). Run yang masih berjalan saat aplikasi ditutup dihentikan tanpa dicatat; langkahnya tetap memegang hasil sebelumnya (bila ada) dan bisa dijalankan ulang.
6. `chat` (opsional): percakapan chat, urut waktu. Pertanyaan: `{id, role: "user", text, at, context?: [id kartu], contextLabel}`; `context` tidak ada = seluruh kanvas. Jawaban: `{id, role: "agent", text, at, replyTo, model, outcome, durationMs, totalTokens, costUsd}`. Pesan dengan `role` lain dilewati saat dimuat.

---

## 5. Integrasi Agent

### 5.1 Keputusan desain

Dibanding draf sebelumnya (dokumen Gemini), ada lima perubahan:

| Topik | Draf sebelumnya | Dokumen ini | Alasan |
|---|---|---|---|
| Penulis file sesi | Aplikasi **dan** MCP server menulis `projects/[id]_session.json`, disinkronkan `QFileSystemWatcher` | Hanya aplikasi yang menulis | Dua penulis berisiko *lost update* dan membaca file setengah tertulis. Dokumentasi Qt juga mencatat: file yang disimpan dengan pola "tulis file baru lalu hapus yang lama" bisa lepas dari pantauan `QFileSystemWatcher`, sehingga harus `addPath()` ulang |
| Hasil stage | Tool `advance_stage` — agent memindahkan task sendiri | Agent melapor lewat `structured_output` (`--json-schema`); `TaskManager` yang memutuskan | Agent tidak bisa melompati gate; aturan transisi ada di satu tempat |
| Gate manusia | Tool `wait_for_approval` menahan agent sampai disetujui | Proses agent **selesai** di akhir stage; *Approve* memicu run baru | Review bisa lama; tool call yang ditahan rawan timeout dan menjaga konteks lama tetap hidup — bertentangan dengan tujuan hemat token |
| Progres live | Tidak dirinci | Dibaca dari stdout `--output-format stream-json` | Tidak perlu tool tambahan |
| Peran MCP | Jalur utama | Ekstensi (M5): baca papan, usulkan subtask, tambah catatan | Loop inti berjalan tanpa MCP; lebih sedikit bagian yang bergerak |

### 5.2 Menjalankan Claude Code per stage

> Sketsa `StageRunner` di bawah adalah rancangan awal. Implementasinya dipecah per tanggung jawab: `ClaudeCli` (argumen), `StreamJsonParser` (event), `ClaudeCodeSession` (proses), `StageSwarm` + `SwarmCoordinator` (antrean & routing) — lihat §3.3. Belum dipakai: `--json-schema`, `--add-dir`, `--max-budget-usd`, `--exclude-dynamic-system-prompt-sections`.

Flag di bawah sudah dicek dengan `claude --help` (Claude Code 2.1.266):

| Flag | Kegunaan |
|---|---|
| `-p` | Non-interaktif; prompt dibaca dari stdin |
| `--output-format stream-json` `--verbose` | Satu event JSON per baris. Tanpa `--verbose`, CLI menolak kombinasi ini |
| `--json-schema <skema>` | Hasil akhir divalidasi dan muncul sebagai `structured_output` di event `result` |
| `--tools <daftar>` | Membatasi tool bawaan per stage → definisi tool di konteks lebih sedikit |
| `--allowedTools <aturan>` | Izin tanpa prompt, mis. `Bash(git *)`, `mcp__swarmforge__*` |
| `--permission-mode acceptEdits` | Edit file tidak perlu konfirmasi |
| `--permission-prompts none` | Tidak ada yang menjawab prompt izin → aksi di luar izin langsung ditolak, proses tidak menggantung |
| `--strict-mcp-config` | Mengabaikan server MCP lain milik pengguna (di M5 ditambah `--mcp-config <run>/mcp.json`) |
| `--add-dir <folder artefak>` | Agent boleh membaca/menulis folder artefak di luar repo |
| `--append-system-prompt <teks>` | Instruksi peran stage |
| `--exclude-dynamic-system-prompt-sections` | Memindahkan bagian yang berubah-ubah (cwd, git status, …) keluar dari system prompt → awal prompt lebih stabil antar run |
| `--max-budget-usd <n>` | Batas biaya per run |
| `--effort <level>` | `low` · `medium` · `high` · `xhigh` · `max` |
| `--model <alias>` | Opsional; contoh alias dari `--help`: `fable`, `opus`, `sonnet` |

Hindari `--bare` (mode itu hanya membaca `ANTHROPIC_API_KEY`/`apiKeyHelper`, jadi login langganan tidak terbaca) dan `--dangerously-skip-permissions`.

```cpp
void StageRunner::start(const TaskItem &task, const StageProfile &profile, const QString &prompt)
{
    const QString claude = QStandardPaths::findExecutable(QStringLiteral("claude"));  // Windows: PATHEXT → claude.exe
    if (claude.isEmpty()) {
        emit failed(task.id, tr("claude tidak ditemukan di PATH"));
        return;
    }

    QStringList args = {
        "-p", "--output-format", "stream-json", "--verbose",
        "--json-schema", compactJson(stageResultSchema()),
        "--tools", profile.tools.join(','),
        "--permission-mode", "acceptEdits",
        "--permission-prompts", "none",
        "--strict-mcp-config",
        "--add-dir", artifactsDir(task),
        "--append-system-prompt", profile.systemPrompt,
        "--exclude-dynamic-system-prompt-sections",
        "--max-budget-usd", QString::number(profile.maxBudgetUsd, 'f', 2),
        "--effort", profile.effort,
    };
    if (!profile.allowedTools.isEmpty())
        args << "--allowedTools" << profile.allowedTools.join(',');

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_project.workingDirectory);
    m_process->setProgram(claude);
    m_process->setArguments(args);

    connect(m_process, &QProcess::started, this, [this, prompt] {
        m_process->write(prompt.toUtf8());      // prompt lewat stdin: aman dari batas panjang command line
        m_process->closeWriteChannel();
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_process->canReadLine())
            handleEvent(QJsonDocument::fromJson(m_process->readLine()).object());
    });
    connect(m_process, &QProcess::finished, this, &StageRunner::onFinished);

    m_timeout.start(profile.timeoutMs);         // QTimer single-shot → m_process->kill()
    m_process->start();
}

void StageRunner::handleEvent(const QJsonObject &event)
{
    const QString type = event.value("type").toString();
    if (type == "system" && event.value("subtype").toString() == "init") {
        m_run.sessionId = event.value("session_id").toString();
        // event ini juga memuat mcp_servers[{name, status}] → cek bridge tersambung (M5)
    } else if (type == "assistant") {
        emit logLine(m_run.stage, summarize(event));   // teks singkat + nama tool yang dipakai
    } else if (type == "result") {
        const QJsonObject usage = event.value("usage").toObject();
        m_run.durationMs      = event.value("duration_ms").toInteger();
        m_run.costUsd         = event.value("total_cost_usd").toDouble();
        m_run.inputTokens     = usage.value("input_tokens").toInteger()
                              + usage.value("cache_creation_input_tokens").toInteger();
        m_run.cacheReadTokens = usage.value("cache_read_input_tokens").toInteger();
        m_run.outputTokens    = usage.value("output_tokens").toInteger();
        m_run.outcome         = event.value("subtype").toString();   // "success", "error_max_budget_usd", …
        m_denials             = event.value("permission_denials").toArray();
        m_result              = event.value("structured_output").toObject();
    }
}
```

> Nama field event mengikuti Claude Code 2.1.266. Sebelum menulis parser, simpan satu output nyata ke `docs/samples/stream-json.jsonl` dan jadikan fixture test — format bisa berubah antar versi CLI.

**Skema hasil stage** (nilai `--json-schema`):

```json
{
  "type": "object",
  "properties": {
    "status":        { "type": "string", "enum": ["completed", "blocked"] },
    "summary":       { "type": "string" },
    "artifacts":     { "type": "array", "items": { "type": "string" } },
    "openQuestions": { "type": "array", "items": { "type": "string" } }
  },
  "required": ["status", "summary", "artifacts", "openQuestions"],
  "additionalProperties": false
}
```

`status: "blocked"` dipakai agent bila tidak bisa lanjut tanpa jawaban manusia. Task langsung masuk `AwaitingReview` dan `openQuestions` tampil di ReviewDialog; jawaban diberikan lewat *Reject* + catatan.

**Prompt dibagi dua:**

- **Instruksi peran** (`--append-system-prompt`) diambil dari `Resources/prompts/<STAGE>.md` dan isinya **tetap**. Jangan sisipkan waktu, ID, atau data task — teks yang identik antar run membantu cache prompt.
- **Isi task** (stdin) dirakit per run:

```markdown
# Task
Judul: {{title}}
Kategori: {{category}}

{{description}}

# Hasil stage sebelumnya ({{previousStage}})
{{previousSummary}}
Artefak: {{previousArtifacts}}

# Catatan review terakhir
{{reviewNote}}
```

### 5.3 Profil stage (titik awal)

| Stage | `--tools` | `--allowedTools` tambahan | Gate default | `--effort` | `maxConcurrent` |
|---|---|---|---|---|---|
| `SPECIFIER` | Read, Grep, Glob | — | **Ya** | high | 3 |
| `CODER` | Read, Grep, Glob, Edit, Write, Bash | `Bash(git *)` + perintah build/test proyek | Tidak | high | 2 |
| `CLEANER` | Read, Grep, Glob, Edit, Bash | perintah test/coverage | Tidak | medium | 1 |
| `ARCHITECT` | Read, Grep, Glob | — | Tidak | high | 2 |
| `HARDENER` | Read, Grep, Glob, Edit, Write, Bash | perintah test | Tidak | high | 1 |
| `QA` | Read, Grep, Glob, Bash | perintah test | **Ya** | medium | 1 |

Semua nilai ini titik awal untuk diukur, bukan aturan. Model default = default CLI. Nilainya ada di `StageCatalog::standard()`; instruksi peran di `Resources/prompts/<STAGE>.md`.

- `maxConcurrent` = ukuran gerombolan stage: run paralel maksimal; sisanya antre FIFO.
- Agent dengan Edit/Write/Bash dianggap penulis: `WorkspaceGuard` hanya mengizinkan satu penulis per folder kerja, lintas stage.
- `SPECIFIER` dan `ARCHITECT` sementara tanpa `Write` (folder artefak belum ada), jadi keduanya read-only dan bebas paralel.

### 5.4 Metrik, biaya, cache

- **Kartu:** `durasi · total token`, dengan total = `inputTokens + cacheReadTokens + outputTokens`. Tooltip berisi rinciannya dan `costUsd`.
- **ReviewDialog:** ringkasan, `openQuestions`, daftar artefak (bisa dibuka), `permission_denials` (aksi agent yang ditolak — sering menjelaskan kenapa hasil kurang), metrik run, dan `sessionId` untuk `claude --resume`.
- **Cache:** bila `cacheReadTokens` selalu 0 pada run berulang untuk stage yang sama, ada bagian awal prompt yang berubah-ubah (instruksi peran, daftar tool, atau server MCP).
- **Biaya dinilai per task yang selesai**, bukan per run: run murah yang sering di-*Reject* atau *Failed* justru lebih mahal. Sebelum mengganti model untuk suatu stage, turunkan `--effort` dulu dan ukur hasilnya.

### 5.5 Keamanan

- `-p` melewati dialog *workspace trust*, jadi agent hanya dijalankan di `workingDirectory` yang didaftarkan pengguna secara eksplisit.
- Tanpa `bypassPermissions`: izin dibatasi `--tools` + `--allowedTools`, sisanya ditolak oleh `--permission-prompts none`.
- Cancel memakai `QProcess::kill()`. Menurut dokumentasi Qt, `terminate()` tidak menghentikan aplikasi konsol di Windows.
- (M5) *Run key* acak per run, `QLocalServer::UserAccessOption`, dan `mcp.json` dihapus setelah run (berisi run key). Bridge harus keluar sendiri saat stdin-nya ditutup.

### 5.6 Bridge MCP (M5)

MCP hanya dipakai untuk kebutuhan yang memang butuh akses ke papan selagi agent bekerja.

| Tool | Parameter | Hasil | Aturan |
|---|---|---|---|
| `get_board` | — | Daftar ringkas task di proyek ini: id, judul, stage, state | Read-only; tanpa deskripsi (hemat token) |
| `get_task` | `task_id` | Detail satu task di proyek yang sama + ringkasan & artefak run terakhirnya | Read-only |
| `propose_subtasks` | `items[]`: `title`, `description`, `category` | Jumlah draf yang dibuat | Task baru masuk `WAITING` sebagai **draf**; manusia yang mengonfirmasi |
| `add_note` | `text` (≤ 2.000 karakter) | `ok` | Catatan tampil di ReviewDialog task yang sedang berjalan |

Prinsip:

- Task yang sedang berjalan **bukan** parameter tool. Bridge membacanya dari env `SWARMFORGE_TASK_ID`, jadi agent tidak bisa mengubah task lain.
- Daftar tool kecil dan tetap → awal prompt stabil.
- Error dikembalikan sebagai error tool yang bisa ditindaklanjuti, mis. `INVALID_STATE: task tidak sedang Running`.
- Tidak ada `advance_stage`, `wait_for_approval`, atau `report_progress` (alasannya di §5.1).

**Protokol aplikasi ↔ bridge**

- Transport: `QLocalServer` (named pipe di Windows) bernama `swarmforge-<pid>`, dengan `setSocketOptions(QLocalServer::UserAccessOption)`.
- Framing: JSON Lines UTF-8; satu koneksi = satu request + satu response.
- Autentikasi: *run key* acak 32 byte per run (`QRandomGenerator::system()`), wajib ada di setiap request; hangus saat run selesai.
- Kode error: `UNAUTHORIZED`, `NOT_FOUND`, `INVALID_STATE`, `BAD_REQUEST`.

```json
{"id": 7, "key": "9f2c…", "method": "add_note", "params": {"taskId": "3f6c…", "text": "Asumsi: papan 3×3"}}
{"id": 7, "ok": true, "result": {}}
{"id": 7, "ok": false, "error": {"code": "INVALID_STATE", "message": "Task tidak sedang Running"}}
```

```cpp
void BridgeServer::listen()
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.listen(QStringLiteral("swarmforge-%1").arg(QCoreApplication::applicationPid()));

    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine())
                    return;                                   // tunggu satu baris utuh
                const QJsonObject request = QJsonDocument::fromJson(socket->readLine()).object();
                socket->write(QJsonDocument(handle(request)).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
        }
    });
}
```

**Konfigurasi per run** (`runs/<taskId>/<STAGE>-<n>/mcp.json`, dipakai dengan `--mcp-config` + `--allowedTools "mcp__swarmforge__*"`):

```json
{
  "mcpServers": {
    "swarmforge": {
      "command": "python",
      "args": ["C:/Tools/SwarmForge/swarmforge_mcp.py"],
      "env": {
        "SWARMFORGE_PIPE": "swarmforge-4812",
        "SWARMFORGE_TASK_ID": "3f6c1e0a-6a0e-4a8e-9a53-2f1f0c9d7b21",
        "SWARMFORGE_RUN_KEY": "9f2c…"
      }
    }
  }
}
```

**Sketsa bridge** (Python, paket `mcp`):

```python
"""swarmforge_mcp.py — bridge MCP stdio yang stateless: setiap tool = satu request ke aplikasi."""
import itertools
import json
import os

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel

PIPE = rf"\\.\pipe\{os.environ['SWARMFORGE_PIPE']}"  # Windows: QLocalServer = named pipe
TASK_ID = os.environ["SWARMFORGE_TASK_ID"]
RUN_KEY = os.environ["SWARMFORGE_RUN_KEY"]
_ids = itertools.count(1)

mcp = FastMCP("swarmforge")


class Subtask(BaseModel):
    title: str
    description: str
    category: str = "component"


def call(method: str, **params):
    request = {"id": next(_ids), "key": RUN_KEY, "method": method,
               "params": {"taskId": TASK_ID, **params}}
    with open(PIPE, "r+b", buffering=0) as pipe:
        pipe.write(json.dumps(request).encode("utf-8") + b"\n")
        response = json.loads(pipe.readline())
    if not response["ok"]:
        error = response["error"]
        raise RuntimeError(f"{error['code']}: {error['message']}")
    return response["result"]


@mcp.tool()
def get_board() -> list[dict]:
    """Daftar ringkas task di proyek ini (id, judul, stage, state). Pakai untuk melihat pekerjaan terkait."""
    return call("get_board")


@mcp.tool()
def get_task(task_id: str) -> dict:
    """Detail satu task di proyek yang sama, termasuk ringkasan dan artefak run terakhirnya."""
    return call("get_task", targetTaskId=task_id)


@mcp.tool()
def propose_subtasks(items: list[Subtask]) -> dict:
    """Usulkan pemecahan task ini menjadi beberapa task baru. Task dibuat sebagai draf dan menunggu konfirmasi manusia."""
    return call("propose_subtasks", items=[item.model_dump() for item in items])


@mcp.tool()
def add_note(text: str) -> str:
    """Tambahkan catatan untuk reviewer: asumsi, risiko, atau pertanyaan. Maksimal 2000 karakter."""
    call("add_note", text=text)
    return "ok"


if __name__ == "__main__":
    mcp.run()  # transport stdio
```

---

## 6. Design System

### 6.1 Prinsip visual

- Terang dan hangat: krem, coklat, navy — mengikuti palet yang sudah ada di `styles.qss`.
- Warna membawa makna (aksi, status), bukan dekorasi.
- Status tidak boleh dibedakan dengan warna saja; selalu ada teks (`RUNNING`, `REVIEW`, `GAGAL`).
- Semua teks memenuhi WCAG AA (≥ 4.5:1); garis/penanda status non-teks ≥ 3:1.
- Satu sumber gaya: `Resources/styles.qss`. Tidak ada `styleSheet` inline di `.ui`.

### 6.2 Tata letak layar

```text
┌────────────────────────────────────────────────────────────────────────────────────┐
│ SwarmForge                                           [New Project]  [Open Project] │
├────────────────────────────────────────────────────────────┬───────────────────────┤
│ ┌─ TTT ──────────────────────────── [New Task] [Close] ──┐ │ [LIEUTENANT]   ● Live │
│ │ WAITING  SPECIFIER  CODER  CLEANER  ARCHITECT  …  DONE │ │                       │
│ │ ┌──────┐          ┌──────┐                             │ │ 10:02:11  SYSTEM      │
│ │ │kartu │          │kartu │                             │ │ 10:02:40  TASK        │
│ │ └──────┘          └──────┘                             │ │ 10:03:02  AGENT:CODER │
│ └────────────────────────────────────────────────────────┘ │                       │
│ ┌─ spacewar ─────────────────────── [New Task] [Close] ──┐ │                       │
│ │ WAITING  SPECIFIER  CODER  CLEANER  ARCHITECT  …  DONE │ │                       │
│ │ ┌──────┐                                               │ │                       │
│ │ │kartu │                                               │ │                       │
│ │ └──────┘                                               │ │ ┌───────────────────┐ │
│ └────────────────────────────────────────────────────────┘ │ │ > perintah…       │ │
│  ↕ scroll vertikal                                         │ └───────────────────┘ │
└────────────────────────────────────────────────────────────┴───────────────────────┘
```

| Elemen | Ukuran | Sumber |
|---|---|---|
| Jendela awal | 1440 × 850 | `main.cpp:23` |
| Top bar | tinggi min. 44 | `mainwindow.ui` |
| Panel konsol | lebar 320–400 | `mainwindow.ui` |
| Swimlane | min. 900 × 300, tinggi tetap; kartu di-scroll per kolom | `SwimlaneWidget.ui` |
| Kartu | desain 245 × 106; lebar nyata mengikuti kolom | `KanbanCardWidget.ui` |

### 6.3 Token warna

**Dasar**

| Token | Nilai | Pemakaian | Perubahan |
|---|---|---|---|
| `bg-app` | `#f8f4ee` | Latar jendela | — |
| `surface-card` | `#ffffff` | Kartu, top bar, tombol sekunder, input | — |
| `surface-lane` | `#fbf8f3` | Latar swimlane | baru (ganti `#f7f9f8` dari `.ui`) |
| `surface-column` | `#f1e8da` | Latar kolom | — |
| `surface-log` | `#fffdf9` | Kotak log konsol | baru (ganti `#fafbfb` dari `.ui`) |
| `border-card` | `#e7ddcd` | Garis kartu | — |
| `border-column` | `#e2d5c1` | Garis kolom, swimlane, top bar | swimlane & top bar ganti `#d3d8d5` |
| `text-default` | `#3d3730` | Teks dasar, judul swimlane | judul swimlane ganti `#111827` |
| `text-muted` | `#6f6557` | Subteks kartu, keterangan | **ganti `#8a7f70`** (kontras) |
| `scrollbar` | `#dccbb4` | Handle scrollbar | — |

**Aksen**

| Token | Nilai | Pemakaian | Perubahan |
|---|---|---|---|
| `primary` | `#8f5f31` | Latar tombol utama | **ganti `#a9743f`** (kontras) |
| `primary-hover` | `#7a5024` | Hover tombol utama | **ganti `#c08a52`** (kontras) |
| `primary-pressed` | `#6a441f` | Tombol utama ditekan | ganti `#8f5f31` |
| `primary-text` | `#8a5a2f` | Judul stage, teks tag kategori | **judul stage ganti `#a9743f`** (kontras) |
| `primary-subtle` | `#f2e3d1` | Latar tag kategori | — |
| `accent` | `#a9743f` | Hanya non-teks: garis hover kartu, scrollbar hover | — |
| `secondary` | `#33517a` | Judul kartu, teks & garis hover tombol sekunder | — |
| `secondary-text` | `#2f4b73` | Teks badge | — |
| `secondary-subtle` | `#e3ebf6` | Latar badge | — |
| `secondary-border` | `#b9c6da` | Garis tombol sekunder | — |
| `badge-border` | `#ccd9ea` | Garis badge | — |
| `secondary-hover-bg` | `#eef2f8` | Hover tombol sekunder | — |
| `secondary-pressed-bg` | `#dfe7f2` | Tombol sekunder ditekan | — |
| `focus` | `#8f5f31` | Penanda fokus keyboard | **ganti hijau `#059669`** dari `.ui` |

**Status** (baru)

| State | Chip | Teks | Latar chip | Garis kartu | Kontras teks |
|---|---|---|---|---|---|
| running | `RUNNING` | `#33517a` | `#e3ebf6` | `#33517a` | 6.73:1 |
| review | `REVIEW` | `#7a4a12` | `#fbeccf` | `#b7791f` | 6.39:1 |
| failed | `GAGAL` | `#9b2c1f` | `#f8e1dc` | `#b4442f` | 6.05:1 |
| done | `DONE` | `#3f6b3a` | `#e3eedf` | `#5b8a55` | 5.20:1 |

Kontras garis kartu terhadap latar putih: 8.08 · 3.64 · 5.51 · 4.03 — semuanya ≥ 3:1.

**Audit kontras** (rumus luminans WCAG 2.x; AA teks normal = 4.5:1)

| Pasangan | Sekarang | Rasio | Usulan | Rasio baru |
|---|---|---|---|---|
| Subteks kartu di atas putih | `#8a7f70` | 3.92 ❌ | `#6f6557` | 5.71 ✅ |
| Judul stage di atas kolom `#f1e8da` | `#a9743f` | 3.29 ❌ | `#8a5a2f` | 4.83 ✅ |
| Teks putih di tombol utama | `#a9743f` | 3.99 ❌ | `#8f5f31` | 5.46 ✅ |
| Teks putih di tombol utama (hover) | `#c08a52` | 3.00 ❌ | `#7a5024` | 7.00 ✅ |
| Judul kartu `#33517a` di atas putih | — | 8.08 ✅ | tetap | — |
| Tag kategori `#8a5a2f` di atas `#f2e3d1` | — | 4.65 ✅ | tetap | — |
| Badge `#2f4b73` di atas `#e3ebf6` | — | 7.36 ✅ | tetap | — |

`text-muted` baru juga lolos di latar lain: 5.22 di `bg-app`, 4.71 di `surface-column`.

### 6.4 Tipografi

Font dasar mengikuti sistem (Segoe UI di Windows). Semua ukuran ditulis di QSS dalam **px** — jangan campur dengan `pointsize` di `.ui` (S9).

| Token | Ukuran · berat | Dipakai |
|---|---|---|
| `font-caption` | 10px · bold | Tag kategori, badge, chip status, judul stage (UPPERCASE, `letter-spacing: 0.5px`) |
| `font-small` | 11px · normal | Subteks kartu (**naik** dari 10px) |
| `font-body` | 11px · normal / bold | Tombol (bold), input, isi dialog |
| `font-card-title` | 12px · bold | Judul kartu |
| `font-lane-title` | 14px · 600 | Judul swimlane (sekarang 11pt di `.ui`, ±15px) |
| `font-app-name` | 16px · bold | Nama aplikasi (sekarang 12pt di `.ui`) |
| `font-mono` | 11px · normal | Log & input konsol: `"Cascadia Code", "Consolas", monospace` |

### 6.5 Spasi & radius

Skala spasi kelipatan 4px:

| Token | Nilai | Pemakaian | Sekarang |
|---|---|---|---|
| `space-1` | 4px | Margin daftar kartu, padding tag | 4 ✅ |
| `space-2` | 8px | Jarak antar kartu, antar kolom, antar elemen header | 6 → 8 |
| `space-3` | 12px | Margin & jarak area konten, jarak antar swimlane | 10 → 12 |
| `space-4` | 16px | Margin kiri-kanan top bar | 16 ✅ |

| Token | Nilai | Pemakaian |
|---|---|---|
| `radius-sm` | 3px | Tag, badge, chip |
| `radius-md` | 4px | Tombol, input, kotak log |
| `radius-lg` | 6px | Kartu |
| `radius-xl` | 8px | Kolom, swimlane |

### 6.6 Komponen

#### Tombol

| Varian | Dipakai untuk | Normal | Hover | Ditekan |
|---|---|---|---|---|
| `primary` | *New Task*, *New Project*, *Approve* | latar `primary`, teks putih | `primary-hover` | `primary-pressed` |
| `secondary` | *Close*, *Open Project*, *Reject*, *Cancel* | latar `surface-card`, teks `secondary`, garis `secondary-border` | latar `secondary-hover-bg`, garis `secondary` | `secondary-pressed-bg` |

Umum: radius 4px, padding 4px 10px, 11px bold, tinggi min. 24px, kursor tangan, penanda fokus keyboard 2px `focus`. Varian dipasang lewat **dynamic property** `variant`, bukan aturan per `objectName`. Di Qt Designer: pilih tombol → *Property Editor* → tombol **+** → *String* → nama `variant`, nilai `primary` atau `secondary`.

#### Kartu

```text
┌──────────────────────────────────┐
│ [component]       RUNNING  [✓ 1] │  ← labelCategory · labelState (baru) · labelBadge
│ unbeatable-ai                    │  ← labelTitle (1 baris, elide + tooltip)
│ 1m 12s · 107k tok                │  ← labelSubtext
└──────────────────────────────────┘
```

| State | Tampilan | Interaksi |
|---|---|---|
| Idle | Garis 1px `border-card`; subteks = metrik run terakhir atau `menunggu` | Drag, Run |
| Hover | Garis 1px `accent` | — |
| Dragging | Kartu asal disembunyikan; *ghost* 75% (dengan DPR yang benar, K9) | — |
| Running | Garis 1px running; chip `RUNNING`; subteks = durasi berjalan (diperbarui tiap detik) | Cancel; tidak bisa di-drag |
| AwaitingReview | Garis **2px** review; tombol 📋 | 📋 / klik kartu → drawer dengan panel keputusan; tidak bisa di-drag |
| Failed | Garis 1px failed; chip `GAGAL`; subteks = alasan singkat | Retry, drag |

- Judul satu baris; teks panjang dipotong dengan `QFontMetrics::elidedText` + tooltip judul lengkap (data mock `"geometry-epoc..."` sekarang dipotong manual).
- Format subteks: `1m 12s · 107k tok`.
- Di `KanbanCardWidget.ui`: tambahkan `QLabel` `labelState` di baris 0 antara spacer dan badge (tersembunyi saat `Idle`); ubah `frame` menjadi `frameShape = NoFrame` (S8).
- State dipasang lewat dynamic property `state` (§6.8).

#### Kolom

- Judul: key stage UPPERCASE, `font-caption`, `primary-text`, plus jumlah kartu: `CODER · 2`.
- Isi: `QScrollArea` tanpa bingkai; daftar kartu margin 4px, jarak 8px, rapat ke atas (stretch di bawah).
- Saat kartu melintas: garis penanda 2px `secondary` di posisi sisip (usulan).
- Latar `surface-column`, garis `border-column`, radius 8px.

#### Swimlane

- Header: `labelProjectTitle` (`font-lane-title`, `text-default`) · spacer · *New Task* (`primary`) · *Close* (`secondary`).
- *Close* menutup tampilan proyek; data tetap ada di file sesi (setelah M2). Sebelum M2, minta konfirmasi bila swimlane berisi kartu.
- Latar `surface-lane`, garis `border-column`, radius 8px, padding 8px.

#### Konsol

- Header: `labelConsoleTitle` `[LIEUTENANT]` (`font-caption`) · spacer · `labelLiveIndicator`: `● Live` (warna running) saat ada run aktif, `○ Idle` (`text-muted`) bila tidak.
- ✅ Di kanan header ada tombol **pin** (`btnConsolePin`) dan **×** (`btnConsoleClose`). Panel punya tiga keadaan, diatur `SidePanelDock`:
  - **Terpasang** (bawaan): menempati pane kanan; lebarnya digeser lewat handle splitter.
  - **Tab** (pin dilepas): tinggal tab tegak di tepi kanan, berbentuk penanda buku seukuran caption-nya (`Lieutenant`, diputar 90° ke kiri, dibaca dari bawah ke atas). Hover atau klik tab menampilkan panel sementara di kiri rel, menimpa board tanpa menggesernya, sampai kursor meninggalkannya. Tombol pin di panel itu memasangnya kembali selebar semula.
  - **Disembunyikan** (×): panel dan relnya hilang, lebarnya jatuh ke board. **View › Lieutenant** (item bercentang) memunculkannya lagi dalam keadaan terakhirnya, dan juga bisa menyembunyikannya.
- Notifikasi tetap dicatat di ketiga keadaan. Keadaan panel tidak disimpan antar sesi: aplikasi selalu dibuka dengan panel terpasang.
- Log: `QPlainTextEdit` **read-only**, `maximumBlockCount` 5000, `font-mono`, latar `surface-log`.
- Input: placeholder `Ketik perintah — "help" untuk daftar`, fokus `focus`. Perintah v1: `help`, `clear`; M3: `run <task>`, `cancel <task>`.
- Satu format baris log untuk semua sumber (`SYSTEM`, `TASK`, `AGENT:<STAGE>`, `GATE`, `ERROR`):

```text
10:02:11  SYSTEM       Projects loaded: TTT, spacewar
10:02:40  TASK         TTT/unbeatable-ai  WAITING → SPECIFIER
10:03:02  AGENT:CODER  Edit src/board.cpp
10:05:47  GATE         TTT/unbeatable-ai menunggu review (QA · 2m 45s · 107k tok)
10:06:10  ERROR        spacewar/shot-acquisition gagal: timeout 20m
```

#### Top bar

- `labelAppName` (`font-app-name`) · spacer · *New Project* (`primary`) · *Open Project* (`secondary`).
- Latar `surface-card`, garis bawah 1px `border-column`, margin kiri-kanan 16px — ditulis di `styles.qss` dengan selector `#topNavBar` (S5).

### 6.7 Temuan styling

Temuan yang ditandai *terverifikasi* sudah diuji dengan program Qt kecil (Lampiran C).

**S1 — Dua palet saling menimpa.** `styles.qss` memakai krem–coklat–navy, sedangkan stylesheet inline di `.ui` memakai abu–hijau: `mainwindow.ui:41-46` (`#f7f9f8`, `#d3d8d5`), `SwimlaneWidget.ui:31-45` (`#111827`), `ConsolePanelWidget.ui:16-51` (`#fafbfb`, fokus `#059669`). Dokumentasi Qt menyatakan stylesheet milik widget **selalu** menang atas stylesheet aplikasi, apa pun spesifisitasnya.
→ Pindahkan semua aturan ke `styles.qss` dengan token §6.3, lalu kosongkan property `styleSheet` di Designer.

**S2 — Latar & garis widget kustom tidak tergambar.** Aturan `KanbanCardWidget {…}` dan `KanbanColumnWidget {…}` (`styles.qss:23-31`, `68-72`) serta `#SwimlaneWidget {…}` tidak menggambar apa-apa, karena subclass `QWidget` tidak otomatis melukis latar dari stylesheet. *Terverifikasi:* tanpa atribut, yang tampil warna induk; dengan atribut di bawah, warna aturan muncul.
→ Tambahkan di constructor keempat widget kustom (kartu, kolom, swimlane, konsol):

```cpp
setAttribute(Qt::WA_StyledBackground, true);
```

**S3 — `#ConsolePanelWidget` tidak pernah cocok.** `setupUi` milik panel memberi nama objek `ConsolePanelWidget`, tetapi `ui_mainwindow.h` hasil uic langsung menggantinya menjadi `consolePanel` setelah constructor selesai. *Terverifikasi.*
→ Pakai selector tipe: `ConsolePanelWidget { … }`.

**S4 — `#labelConsoleHeader` menunjuk objek yang tidak ada.** Label judul konsol bernama `label` (`ConsolePanelWidget.ui:60`).
→ Ganti nama objek (Lampiran B).

**S5 — Stylesheet `topNavBar` tanpa selector bocor ke anak-anaknya** (`mainwindow.ui:41-46`). *Terverifikasi:* tombol di dalam container yang stylesheet-nya tanpa selector ikut berubah latar.
→ Tulis sebagai `#topNavBar { … }` di `styles.qss`.

**S6 — Stylesheet `columnsContainer` hanya salinan** stylesheet swimlane (`SwimlaneWidget.ui:119-133`) → hapus. Selain itu `.ui` sudah memberi `columnsContainer` sebuah layout (`SwimlaneWidget.ui:134`), sehingga cabang kode yang mengatur jarak 6px dan margin 0 (`SwimlaneWidget.cpp:47-51`) tidak pernah dijalankan.
→ Atur jarak & margin `horizontalLayout` langsung di Designer.

**S7 — Kontras di bawah AA** untuk subteks kartu, judul stage, dan tombol utama → lihat audit di §6.3.

**S8 — Bingkai ganda pada kartu** setelah S2: `QFrame` `frame` bergaya `StyledPanel`/`Raised` (`KanbanCardWidget.ui:18-24`).
→ `frameShape = NoFrame`.

**S9 — Satuan font campur**: `pointsize` di `.ui` (`mainwindow.ui:64`, `SwimlaneWidget.ui:56`) dan px di QSS.
→ Pindahkan ukuran font ke QSS (px).

**S10 — `line-height` tidak didukung Qt Style Sheets** (tidak ada di *List of Properties* Qt 6.11), jadi `ConsolePanelWidget.ui:36` tidak berefek.
→ Hapus.

### 6.8 Menerapkan token di QSS

QSS tidak punya variabel. Tulis token sebagai `@nama` di `styles.qss`, lalu ganti sebelum `setStyleSheet()`:

```cpp
// main.cpp
static QString applyTokens(const QString &qss, const QHash<QString, QString> &tokens)
{
    static const QRegularExpression tokenPattern(QStringLiteral("@([a-z][a-z0-9-]*)"));
    QString result;
    qsizetype last = 0;
    auto it = tokenPattern.globalMatch(qss);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += qss.mid(last, match.capturedStart() - last);
        result += tokens.value(match.captured(1), match.captured(0));  // token tak dikenal dibiarkan apa adanya
        last = match.capturedEnd();
    }
    return result + qss.mid(last);
}

// app.setStyleSheet(applyTokens(QString::fromUtf8(styleFile.readAll()), Theme::tokens()));
```

```css
/* Resources/styles.qss */
KanbanCardWidget {
    background-color: @surface-card;
    border: 1px solid @border-card;
    border-radius: 6px;
}
KanbanCardWidget:hover            { border-color: @accent; }
KanbanCardWidget[state="running"] { border: 1px solid @running-border; }
KanbanCardWidget[state="review"]  { border: 2px solid @review-border; }
KanbanCardWidget[state="failed"]  { border: 1px solid @failed-border; }

QLabel#labelSubtext { font-size: 11px; color: @text-muted; }

QPushButton[variant="primary"] {
    background-color: @primary; color: #ffffff; border: none;
    border-radius: 4px; padding: 4px 10px; font-size: 11px; font-weight: bold;
}
QPushButton[variant="primary"]:hover   { background-color: @primary-hover; }
QPushButton[variant="primary"]:pressed { background-color: @primary-pressed; }

QPushButton[variant="secondary"] {
    background-color: @surface-card; color: @secondary; border: 1px solid @secondary-border;
    border-radius: 4px; padding: 4px 10px; font-size: 11px; font-weight: bold;
}
QPushButton[variant="secondary"]:hover   { background-color: @secondary-hover-bg; border-color: @secondary; }
QPushButton[variant="secondary"]:pressed { background-color: @secondary-pressed-bg; }
```

Nilai dynamic property yang berubah setelah stylesheet terpasang butuh *repolish* (dokumentasi Qt):

```cpp
void KanbanCardWidget::setState(TaskState state)
{
    setProperty("state", stateKey(state));   // "idle" | "running" | "review" | "failed"
    style()->unpolish(this);
    style()->polish(this);
}
```

---

## 7. Temuan pada Kode

Prioritas: **P1** dikerjakan di M0 · **P2** di M1 · **P3** kapan saja.

**K1 · P1 — Pointer mentah di data drag & drop.** `KanbanCardWidget.cpp:85` menulis alamat memori kartu ke MIME, lalu `KanbanColumnWidget.cpp:114-119` membacanya kembali dengan `reinterpret_cast`. Drop dari instance SwarmForge lain (format MIME sama) akan dibaca sebagai alamat di proses ini → crash.
→ Ambil kartu dari `event->source()` — nilainya `nullptr` bila drag berasal dari aplikasi lain — dan isi MIME dengan `taskId`:

```cpp
void KanbanColumnWidget::dropEvent(QDropEvent *event)
{
    auto *card = qobject_cast<KanbanCardWidget *>(event->source());
    if (!card || !event->mimeData()->hasFormat(kTaskMimeType))   // "application/x-swarmforge-task"
        return;

    insertCard(calculateInsertIndex(event->position().toPoint().y(), card), card);  // M1: emit moveRequested(…)
    emit cardDropped(card, m_stageName, m_cardListLayout->indexOf(card));
    event->acceptProposedAction();
}
```

**K2 · P1 — Posisi drop salah di kolom yang sama.** `calculateInsertIndex` (`KanbanColumnWidget.cpp:71-91`) ikut menghitung kartu yang sedang di-drag, padahal `insertWidget()` melepas kartu itu dari layout **sebelum** menyisipkan, sehingga indeks bergeser. *Terverifikasi:*
- `[A, B, C]`, A di-drop di antara B dan C → hasil `B, C, A` (seharusnya `B, A, C`).
- `[P, Q]`, P di-drop di bawah Q → hasil `Q, stretch, P`: P menempel di dasar kolom, di bawah stretch.

→ Hitung posisi di antara kartu **lain** saja:

```cpp
int KanbanColumnWidget::calculateInsertIndex(int dropY, const KanbanCardWidget *dragged) const
{
    int index = 0;
    for (int i = 0; i < m_cardListLayout->count(); ++i) {
        auto *card = qobject_cast<KanbanCardWidget *>(m_cardListLayout->itemAt(i)->widget());
        if (!card || card == dragged)
            continue;
        const int middleY = card->mapTo(this, QPoint(0, 0)).y() + card->height() / 2;
        if (dropY < middleY)
            return index;
        ++index;
    }
    return index;   // tidak pernah melewati stretch
}
```

**K3 · P1 — Huruf besar/kecil nama file.** Di index git tercatat `kanbancolumnwidget.cpp`, sedangkan `.pro` dan disk memakai `KanbanColumnWidget.cpp`. `mainwindow.cpp:1-2` meng-include `MainWindow.h` dan `ui_MainWindow.h`, padahal filenya `mainwindow.h` dan `ui_mainwindow.h`. Build lolos di Windows, tetapi gagal di Linux/macOS/CI.
→ Perbaiki index git, dan ubah include menjadi `"mainwindow.h"` / `"ui_mainwindow.h"`:

```bash
git rm --cached kanbancolumnwidget.cpp
git add KanbanColumnWidget.cpp
```

**K4 · P1 — `Resources/` belum masuk git.** Commit pertama tidak akan berisi `styles.qss` dan `resources.qrc`, sehingga `.pro` gagal di-build dari hasil clone.
→ `git add Resources/`.

**K5 · P2 — Model belum dipakai dan urutan tidak tersimpan.** `TaskManager::updateTaskStage` (`TaskManager.cpp:16-23`) hanya menyimpan stage; `targetIndex` cuma diteruskan lewat sinyal. `tasksForProject` mengembalikan urutan kunci `QMap` (urut id), bukan urutan kolom.
→ M1: `TaskItem` v2 dengan `order`.

**K6 · P2 — Nama stage berupa string literal yang tersebar** (`SwimlaneWidget.cpp:9-12`, `mainwindow.cpp:89` dan `109-135`). `addCardToStage` (`SwimlaneWidget.cpp:68-72`) diam-diam mengabaikan nama yang salah, jadi kartu hilang tanpa pesan.
→ `enum class Stage` (§4.3).

**K7 · P2 — *Close* langsung menghapus swimlane beserta semua kartunya** tanpa konfirmasi (`mainwindow.cpp:47-53`).
→ Konfirmasi sebelum M2; setelah M2, *Close* hanya menutup tampilan.

**K8 · P2 — Kartu bisa pindah antar swimlane**, karena semua kolom menerima format MIME yang sama; task pindah proyek tanpa jejak.
→ `TaskManager::requestMove` menolak bila `projectId` berbeda.

**K9 · P3 — *Ghost* drag buram di layar ber-skala.** `QPixmap ghostPixmap(pixmap.size())` (`KanbanCardWidget.cpp:92`) tidak membawa `devicePixelRatio`. *Terverifikasi:* pada skala 2, hasil `grab()` ber-DPR 2 tetapi ghost ber-DPR 1.
→ Tambahkan `ghostPixmap.setDevicePixelRatio(pixmap.devicePixelRatio());`.

**K10 · P3 — Log konsol bisa diketik.** `QPlainTextEdit` tidak read-only secara default (`ConsolePanelWidget.ui:90`).
→ Centang `readOnly` di Designer.

**K11 · P3 — Fitur yang belum tersambung:** perintah `clear` tidak melakukan apa-apa (`mainwindow.cpp:97-99`); tombol *Open Project* dan sinyal `cardClicked` belum punya penerima.

**K12 · P3 — ID task dari waktu** (`mainwindow.cpp:83`, `currentMSecsSinceEpoch()`).
→ `QUuid::createUuid().toString(QUuid::WithoutBraces)`.

**K13 · P3 — Tidak bisa scroll horizontal.** `scrollAreaSwimlanes` memakai `ScrollBarAlwaysOff` (`mainwindow.ui:129-131`), sementara lebar min. swimlane 900px. Jika lebar jendela di bawah ±1.260px, kolom kanan terpotong tanpa bisa digulir.
→ `ScrollBarAsNeeded`.

**K14 · P3 — Kebersihan header:** `#ifndef` dan `#pragma once` dipakai bersamaan; `Q_OBJECT;` bertitik koma (`KanbanCardWidget.h:16`).
→ Pakai `#pragma once` saja.

---

## 8. Roadmap

| Milestone | Isi | Selesai bila |
|---|---|---|
| **M0 — Fondasi** | K1–K4, K9–K10, S1–S10, token warna | Clone bersih ter-build di mesin lain · drag ke dasar kolom sendiri menghasilkan urutan yang benar · latar kartu/kolom/swimlane tampil sesuai token · semua teks ≥ 4.5:1 |
| **M1 — Model jadi sumber kebenaran** | `Stage`, `TaskItem` v2, `TaskManager` dimiliki `main.cpp`, drag & drop lewat `requestMove`, K5–K8, K12 | Tidak ada `new KanbanCardWidget` di luar handler `taskAdded` · Qt Test untuk aturan transisi (valid, `Running` ditolak, antar-swimlane ditolak) lulus |
| **M2 — Persistensi** | `SessionStore`, *New/Open Project*, autosave debounce | Tutup–buka aplikasi → kartu, urutan, dan riwayat run sama persis · file rusak tidak ditimpa |
| **M3 — StageRunner** | Profil stage, template prompt, parsing stream-json, metrik di kartu, Cancel, timeout, `events.jsonl` | Satu task menjalankan `SPECIFIER` dari awal sampai akhir · kartu menampilkan durasi & token nyata · Cancel menghentikan proses ≤ 2 detik · `subtype` error → `Failed` dengan alasan |
| **M4 — Gate review** | State `AwaitingReview`, ReviewDialog, *Approve*/*Reject* + catatan, gate per proyek | Task berhenti di gate `SPECIFIER` · *Reject* + catatan → run ulang memuat catatan · *Approve* → `Idle` di `CODER` |
| **M5 — Bridge MCP** | `BridgeServer`, `swarmforge_mcp.py`, 4 tool, run key | Agent `SPECIFIER` bisa mengusulkan subtask yang muncul sebagai draf · request dengan key salah ditolak `UNAUTHORIZED` |

---

## 9. Keputusan Terbuka

| # | Pertanyaan | Usulan dokumen ini |
|---|---|---|
| 1 | Apa arti badge `✓ N`? `styles.qss` menyebutnya "badge prioritas", placeholder `.ui` berisi `High`, data mock berisi `✓ 0`–`✓ 2` | **Diputuskan:** jumlah gate yang run terakhirnya disetujui (`TaskItem::approvedGates()`); stepper "APPROVALS REQUIRED" dihapus |
| 2 | Nama produk: judul jendela "SwarmForge", tetapi label top bar "L'Assomoir" (`mainwindow.ui:69`) | Pilih satu dan pakai di keduanya |
| 3 | Palet final | Krem–coklat–navy dari `styles.qss` (dokumen ini mengasumsikannya) |
| 4 | Stage ber-gate default | `SPECIFIER` dan `QA`; bisa diatur per proyek |
| 5 | Setelah *Approve*, stage berikutnya langsung jalan? | **Diputuskan:** kartu maju otomatis ke stage berikutnya, tetapi Run tetap manual (▶) agar biaya terkendali; *Revisi* langsung menjalankan ulang stage yang sama |
| 6 | Lokasi data sesi & artefak | `%APPDATA%\SwarmForge\projects\<id>\`; repo target tetap bersih |
| 7 | Bahasa bridge MCP | Python (FastMCP) dulu; mode `--mcp-stdio` di executable C++ bisa menyusul |
| 8 | Kartu boleh pindah antar swimlane? | Tidak |

---

## Lampiran A: Glosarium

| Istilah | Arti |
|---|---|
| Proyek / swimlane | Satu baris papan = satu repositori target (`projectId`) |
| Stage | Salah satu dari 8 kolom pipeline |
| State | Kondisi task di dalam stage: `Idle`, `Running`, `AwaitingReview`, `Failed` |
| Run | Satu eksekusi `claude -p` untuk satu task di satu stage |
| Gate | Titik wajib review manusia setelah stage tertentu |
| Artefak | File keluaran stage (spesifikasi, laporan QA, …) |
| Intent | Permintaan perubahan ke `TaskManager` yang bisa ditolak |
| Run key | Kunci acak per run untuk autentikasi bridge MCP — **bukan** token LLM |
| Token | Satuan teks yang dihitung dan ditagih model (input, output, cache) |

## Lampiran B: Konvensi

- **Nama file** = nama kelas dalam PascalCase (`TaskManager.h`). File `mainwindow.*` yang sudah ada boleh diganti nama nanti.
- **Header**: `#pragma once` saja.
- **Nama objek**: awalan jenis — `btn…`, `label…`, `lineEdit…`, `plainText…`, `scrollArea…`; wadah berakhiran `…Container`.
- **Sinyal**: peristiwa dalam bentuk lampau (`taskMoved`), permintaan berakhiran `Requested` (`moveRequested`).
- **Gaya**: hanya di `Resources/styles.qss`; varian lewat dynamic property (`variant`, `state`).
- **Log**: `hh:mm:ss  SUMBER  pesan`.

Usulan ganti nama objek:

| Sekarang | Usulan | Alasan |
|---|---|---|
| `label` (konsol) | `labelConsoleTitle` | Nama generik; target S4 |
| `label_2` (konsol) | `labelLiveIndicator` | Nama generik |
| `headerLayout` (konsol) | `consoleHeader` | Sebenarnya `QWidget`, bukan layout |
| `textBrowserLog` | `plainTextLog` | Sebenarnya `QPlainTextEdit`; ubah juga `ConsolePanelWidget.cpp:20` |
| `frame` (kartu) | `cardFrame` | Nama generik |

## Lampiran C: Verifikasi

| Klaim | Cara dicek |
|---|---|
| Flag CLI di §5.2 | `claude --help`, Claude Code 2.1.266 |
| `stream-json` butuh `--verbose`; field event `init`/`result` (`session_id`, `mcp_servers`, `subtype`, `usage`, `total_cost_usd`, `permission_denials`, `structured_output`); format izin `mcp__<server>__*` | String di `claude.exe` 2.1.266 |
| S2, S3, S5, K2, K9 | Program uji kecil dengan Qt 6.11.2 (platform `offscreen`), di luar repo |
| Aturan cascade QSS, daftar properti (`line-height`), `QFileSystemWatcher`, `QSaveFile`, `QProcess::terminate`, `QDropEvent::source`, `QLocalServer`, `QStandardPaths::findExecutable` | Dokumentasi lokal `C:/Qt/Docs/Qt-6.11.2/` |
| Layout `columnsContainer` (S6) dan urutan `setObjectName` (S3) | Header hasil uic di `build/…/ui_SwimlaneWidget.h`, `ui_mainwindow.h` |
| Rasio kontras | Rumus luminans relatif WCAG 2.x |
