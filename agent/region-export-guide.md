# Region-Based Terrain Export Guide

Bu dokuman, editordeki region-based terrain display sisteminin nasil calistigini, region verisinin hangi dosyalardan ogrenildigini, server tarafindaki region islemesini ve ayni mantikla region'larin ayri ayri nasil export edilebilecegini detayli olarak aciklar.

---

## 1. Terrain Dunya Yapisi

### Koordinat Sistemi

```
Dunya origini: (0, 0, 0)
X ekseni: Bati (-) → Dogu (+)
Y ekseni: Asagi (-) → Yukari (+)  (yukseklik)
Z ekseni: Kuzey (-) → Guney (+)

Dunya sinirlari:
  X: [-2,560,000 .. +2,560,000]
  Z: [-2,560,000 .. +2,560,000]
```

### Hiyerarsi

```
Dunya (5,120,000 x 5,120,000 birim)
  └── 800 x 800 Sektor gridi
        └── Her sektor: 6,400 x 6,400 birim
              └── 16 x 16 Segment gridi
                    └── Her segment: 400 x 400 birim
```

### Sabitler

| Sabit | Deger | Kaynak |
|-------|-------|--------|
| `AP_SECTOR_WORLD_INDEX_WIDTH` | 800 | `source/public/ap_sector.h` |
| `AP_SECTOR_WORLD_INDEX_HEIGHT` | 800 | `source/public/ap_sector.h` |
| `AP_SECTOR_WIDTH` | 6400.0 | `source/public/ap_sector.h` |
| `AP_SECTOR_HEIGHT` | 6400.0 | `source/public/ap_sector.h` |
| `AP_SECTOR_DEFAULT_DEPTH` | 16 | `source/public/ap_sector.h` |
| `AP_SECTOR_STEPSIZE` | 400.0 | `source/public/ap_sector.h` |
| `AP_SECTOR_WORLD_START_X` | -2,560,000 | `source/public/ap_sector.h` |
| `AP_SECTOR_WORLD_START_Z` | -2,560,000 | `source/public/ap_sector.h` |
| `AP_MAP_MAX_REGION_ID` | 349 | `source/public/ap_map.h` |
| `AP_MAP_MAX_REGION_COUNT` | 350 | `source/public/ap_map.h` |

### Sektor Indeks ↔ Dunya Pozisyon Donusumleri

```
sektor_dunya_x = AP_SECTOR_WORLD_START_X + sektor_index_x * AP_SECTOR_WIDTH
sektor_dunya_z = AP_SECTOR_WORLD_START_Z + sektor_index_z * AP_SECTOR_HEIGHT

segment_dunya_x = sektor_dunya_x + segment_x * AP_SECTOR_STEPSIZE
segment_dunya_z = sektor_dunya_z + segment_z * AP_SECTOR_STEPSIZE

sektor_index_x = floor((dunya_x - AP_SECTOR_WORLD_START_X) / AP_SECTOR_WIDTH)
sektor_index_z = floor((dunya_z - AP_SECTOR_WORLD_START_Z) / AP_SECTOR_HEIGHT)

segment_x = floor((dunya_x - sektor_extent_start_x) / AP_SECTOR_STEPSIZE)
segment_z = floor((dunya_z - sektor_extent_start_z) / AP_SECTOR_STEPSIZE)
```

---

## 2. Kaynak Dosyalar — Editorun Region Verisini Nereden Ogrendigi

Editor, region ayrimini ogrenmek icin birden fazla dosya kaynagini okur. Asagida her dosyanin yolu, formati ve ne icin kullanildigi aciklanir.

### 2.1 Konfigurasyon Dosyasi

**Dosya:** `./config` (proje kokunde)

```
ServerIniDir=content/server/ini
ServerNodePath=content/server/world/node_data
ClientDir=content/client
EditorStartPosition=-386803.69,6888.24,98187.72
```

Bu dosya `ap_config_get(mod->ap_config, "KeyName")` cagrisiyla okunur. Asagidaki tum dosya yollari bu config degerlerinden turetilir.

### 2.2 node_data — Ana Region/Segment Veritabani

**Dosya:** `content/server/world/node_data` (config'deki `ServerNodePath`)
**Boyut:** ~45 MB
**Okuyan:** `find_region_sectors()` — `source/editor/main.cpp`
**Amac:** Tum dunyadaki segment→region_id eslestirmesini icerir. Region secildiginde ilk tarama bu dosya uzerinden yapilir.

**Binary format:**
```
Dosya = art arda gelen sektor girisleri

Her sektor girisi = 1032 byte:
  [u32] sektor_index_x          (4 byte)
  [u32] sektor_index_z          (4 byte)
  [256 × 4 byte] segment verisi (1024 byte)

Her segment = 4 byte:
  Byte 0-1: tile_info (is_edge_turn:1, tile_type:7, geometry_block:4, has_no_layer:1, reserved:3)
  Byte 2-3: region_id (uint16_t)

Segmentler z-major sirada dizilir:
  segment_index = z * 16 + x  (yani 0..255 arasi, z dis dongu, x ic dongu)

Region ID okuma:
  memcpy(&region_id, (uint8_t*)cursor + 2, 2);
  region_id = region_id & 0x00FF;   // Alt 8 bit kullanilir
```

**Not:** node_data dosyasi sadece segment iceren sektorleri barindirmak zorunda degildir — dosyadaki sektor sayisi degiskendir.

### 2.3 Compact Segment Dosyalari (.amf)

**Dosya yolu:** `{ClientDir}/world/C{x:03u},{z:03u}.amf`
**Ornek:** `content/client/world/C392,393.amf`
**Okuyan:** `segments_from_stream()` — `source/client/ac_terrain.c`
**Amac:** Sektor yuklendiginde segment verisini saglar. node_data'dan daha guncel olabilir (editor segmentleri degistirip kaydettiginde .amf guncellenir).

**Binary format (VERSION 4 — guncel):**
```
[u32] version = 0x1003 (ALEF_COMPACT_FILE_VERSION4)
[u32] flags
[u32] depth = 16
[256 × sizeof(ac_terrain_segment)] segment verisi — z-major sirada
[u32] line_block_count
[line_block_count × sizeof(line_block)] line block verisi
```

**Versiyon farklari:**
| Versiyon | Deger | Fark |
|----------|-------|------|
| V1 | 0x1000 | Temel format, region_id `& 0x00FF` maskelenir |
| V2 | 0x1001 | +segment flags alani |
| V3 | 0x1002 | Raw struct okuma (maskeleme yok), transpose gerekli |
| V4 | 0x1003 | +line block verisi |

**Kritik — Bellek vs Dosya sirasi farki:**
```
Dosyada:   z-major (dis dongu z, ic dongu x) → buf[z * 16 + x]
Bellekte:  segments[x][z] — X birinci indeks, Z ikinci indeks

Yukleme sirasinda transpose yapilir:
  for (x = 0; x < 16; x++)
    for (z = 0; z < 16; z++)
      segments[x][z] = buf[z * 16 + x];
```

### 2.4 Packed Terrain Arsivleri (.ma1 / .ma2)

**Dosya yolu ornekleri:**
```
content/client/world/a00{xx}{zz}x.ma1   — segment verisi (sikistirilmis)
content/client/world/a00{xx}{zz}x.ma2   — rough geometry
content/client/world/b00{xx}{zz}x.ma2   — detail geometry
```

**Okuyan:** `unpack_terrain_file()` ve `load_segments()` — `source/client/ac_terrain.c`
**Amac:** Terrain geometrisi (vertex, ucgen, material) ve sikistirilmis segment verisini icerir.

**Not:** .amf dosyasi varsa, .ma1'den okunan segment verisi yerine .amf tercih edilir. .amf editorden kaydedilmis guncel veriyi tasir.

### 2.5 Region Template Tanimlari

**Dosya:** `content/server/ini/regiontemplate.ini` (config'deki `ServerIniDir` + `/regiontemplate.ini`)
**Okuyan:** `ap_map_read_region_templates()` — `source/public/ap_map.c`
**Amac:** Her region_id icin isim, ozellikler ve oyun kurallarini tanimlar.

**Format (INI):**
```ini
[42]
Name=Archlord_Plains
ParentIndex=-1
ItemSection=0
Type=256
Comment=Archlord Plains bolgesi
ResurrectionX=-386803.0
ResurrectionZ=98187.0
WorldMap=1
SkySet=0
VisibleDistance=30000.0
TopViewHeight=5000.0
LevelLimit=0
LevelMin=1
EnumEnd
```

Bu dosya region ID → isim eslestirmesini saglar. Editorde region listesi gosterilirken bu veriler kullanilir.

### 2.6 Region Glossary

**Dosya:** `content/server/ini/regionglossary.txt`
**Okuyan:** `ap_map_read_region_glossary()` — `source/public/ap_map.c`
**Amac:** Region isimlerinin kullaniciya gorunur etiketlerini tanimlar.

**Format (Tab-separated):**
```
RegionInternalName	Gorunen Etiket
Archlord_Plains	Archlord Ovalari
```

### 2.7 Dosya Okuma Akisi — Ozet

```
Editor baslatildiginda:
  1. ./config okunur → dosya yollari belirlenir
  2. regiontemplate.ini okunur → region tanimlari yuklenir (isim, ozellikler)
  3. regionglossary.txt okunur → gorunen etiketler yuklenir

Kullanici region sectiginde:
  4. node_data okunur → tum dunyadaki eslesen segmentler taranir
  5. Eslesen sektorlerin konumu ve GPU mask hesaplanir
  6. Kamera centroid'e konumlandirilir

Sektorler yuklenirken:
  7. .amf dosyasi varsa → compact segment verisi okunur
  8. .amf yoksa → .ma1 arsivinden segment verisi cikarilir
  9. .ma2 arsivlerinden geometry (vertex, ucgen, material) yuklenir
  10. rebuild_segment_mask() cagirilir → GPU mask .amf verisinden yeniden olusturulur
```

---

## 3. Region Veri Yapisi

### Segment Yapisi

Her segment asagidaki veriyi tasir:

```c
// source/client/ac_terrain.h (client-side)
struct ac_terrain_segment {
    struct ac_terrain_tile_info tile;  // 2 byte
    uint16_t region_id;               // 2 byte — hangi region'a ait
};

// source/server/as_map.h (server-side — ayni yapi)
struct as_map_segment {
    struct as_map_tile_info tile;     // 2 byte
    uint16_t region_id;               // 2 byte
};
```

Bir sektorun tum segmentleri:

```c
struct ac_terrain_segment_info {
    uint32_t flags;
    struct ac_terrain_segment segments[16][16];  // segments[x][z]
    struct ac_terrain_line_block * line_blocks;
    uint32_t line_block_count;
};
```

**Kritik: Dizilim `segments[x][z]` seklindedir** — ilk indeks X, ikinci indeks Z. Bu `get_segment_by_pos()` fonksiyonundan dogrulanmistir.

### Region ID Atamasi

- Her 400x400 birimlik segment TAM OLARAK BIR region'a aittir.
- `region_id` 16-bit deger, ancak eski dosya formatlarinda alt 8 bit (`& 0x00FF`) kullanilir.
- `region_id == 0` veya gecersiz degerler: region'a atanmamis segment.
- Bir sektor icerisinde farkli segmentler farkli region'lara ait olabilir.

### Region Template Yapisi

```c
// source/public/ap_map.h
struct ap_map_region_template {
    boolean in_use;
    uint32_t id;                                         // Region ID (0-349)
    int32_t parent_id;
    char name[AP_MAP_MAX_REGION_NAME_SIZE];               // Dahili isim
    char comment[AP_MAP_MAX_REGION_NAME_SIZE];
    char glossary[AP_MAP_MAX_REGION_NAME_SIZE];            // Gorunen etiket
    union ap_map_region_type type;                         // Oyun ozellikleri
    uint32_t peculiarity;                                  // Ozel kisitlamalar
    uint32_t world_map_index;
    uint32_t sky_index;
    float visible_distance;
    float max_camera_height;
    uint32_t level_limit;
    uint32_t level_min;
    uint32_t disabled_item_section;
    float resurrection_pos[2];                             // Dirilis noktasi X, Z
    float zone_src[2];                                     // Zon gecis kaynak
    float zone_dst[2];                                     // Zon gecis hedef
    float zone_height;
    float zone_radius;
};
```

### Region Ozellikleri (Bit Alanlari)

```c
struct ap_map_region_properties {
    uint8_t field_type : 2;        // 0=Alan, 1=Kasaba, 2=PvP
    uint8_t safety_type : 2;       // 0=Guvenli, 1=Serbest, 2=Tehlikeli
    uint8_t is_ridable : 1;        // Binek kullanimi
    uint8_t allow_pets : 1;
    uint8_t allow_round_trip : 1;
    uint8_t allow_potion : 1;
    uint8_t allow_resurrection : 1;
    uint8_t disable_minimap : 1;
    uint8_t is_jail : 1;
    uint8_t zone_load_area : 1;
    uint8_t block_characters : 1;
    // ... diger flagler
};
```

### Region Pozisyonlari — Dunya Uzerinde Nasil Konuslandirilmis

Region'larin dunya uzerindeki konumlari, ayri bir "region boundary" veya "region polygon" dosyasinda tanimlanmaz. Region pozisyonlari **tamamen segment atamalarindan turetilir:**

**Tek kaynak: `node_data` dosyasi** (`content/server/world/node_data`)

Bu dosyadaki her segment (400x400 birimlik alan) bir `region_id` tasir. Bir region'un dunya uzerindeki alani, o `region_id`'ye atanmis tum segmentlerin birlesiminden olusur. Yani:

```
Region 42'nin alani = { tum segmentler S : S.region_id == 42 }
```

**Ozellikleri:**
- Bir region'un siniri duzensiz olabilir — dikdortgen olmak zorunda degildir.
- Region alani bitisik olmak zorunda degildir — ayri adalarin hepsi ayni region_id'ye sahip olabilir.
- Komsuluk bilgisi yoktur — hangi region'larin yanyana oldugu, segment atamalarindan cikarilabilir ama ayrica saklanmaz.
- Bir segment yalnizca BIR region'a aittir — cakisma yoktur.

**Region template dosyasindaki pozisyon bilgileri:**

`regiontemplate.ini` dosyasinda region'larin birkaç nokta koordinati vardir, ancak bunlar region sinirlarini degil, oyun mekaniği noktalarini tanimlar:

| Alan | Aciklama | Kullanim |
|------|----------|----------|
| `ResurrectionX`, `ResurrectionZ` | Dirilis noktasi | Karakter olunce bu noktada canlanir |
| `ZoneSrcX`, `ZoneSrcZ` | Zon gecis kaynak noktasi | Zon gecis mekaniği |
| `ZoneDstX`, `ZoneDstZ` | Zon gecis hedef noktasi | Zon gecis mekaniği |
| `ZoneRadius`, `ZoneHeight` | Zon gecis alani boyutlari | Zon gecis mekaniği |

Bu koordinatlar region'un geometrik sinirlarini TANIMLAMAZ — yalnizca oyun icinde kullanilan referans noktalardir.

**Region konumu nasil hesaplanir (editorde):**

```
1. node_data dosyasi taranir
2. target_region_id ile eslesen tum segmentler bulunur
3. Her eslesen segmentin dunya merkezi hesaplanir:
     segment_merkez_x = WORLD_START_X + sektor_x * 6400 + segment_x * 400 + 200
     segment_merkez_z = WORLD_START_Z + sektor_z * 6400 + segment_z * 400 + 200
4. Tum merkez noktalarinin ortalamasi = region centroid
5. Centroid etrafindaki sektorler yuklenir
```

**Ozet:** Region'un "nerede oldugunu" ogrenmek icin tek yapilacak sey `node_data` dosyasindaki segment→region_id eslestirmelerini taramak. Region siniri, bu segmentlerin geometrik birlesiminden ortaya cikar.

---

## 4. Server Tarafinda Region Islemesi

### 4.1 Server Veri Yapisi

Server, tum dunyanin sektor ve segment verisini bellekte tutar:

```c
// source/server/as_map.h
struct as_map_sector {
    uint32_t index_x;
    uint32_t index_z;
    struct au_pos begin;                    // Sektor baslangic pozisyonu
    struct au_pos end;                      // Sektor bitis pozisyonu
    struct as_map_segment segments[16][16]; // 16x16 segment gridi [x][z]
    struct ap_character * characters;       // Sektordeki karakterler (linked list)
    struct ap_object ** objects;            // Sektordeki objeler (vector)
    struct as_map_item_drop * item_drops;   // Yerdeki itemler
};

struct as_map_region {
    boolean initialized;
    struct ap_map_region_template * temp;   // Region template referansi
    struct ap_character ** npcs;            // Bu regiondaki NPC'ler (vector)
};

struct as_map_module {
    struct as_map_region regions[AP_MAP_MAX_REGION_COUNT];  // 350 region
    struct as_map_sector * sectors;   // 800x800 sektor gridi (bellekte flat array)
    // ...
};
```

### 4.2 Server Baslatma Sirasi

```
1. ap_map_read_region_templates()    → regiontemplate.ini okunur
     Her template icin AP_MAP_CB_INIT_REGION callback tetiklenir
     → as_map_module.regions[id] baslatilir, NPC vector olusturulur

2. ap_map_read_region_glossary()     → regionglossary.txt okunur

3. as_map_read_segments()            → node_data okunur
     Tum 800x800 sektor gridine segment verisi yuklenir
     Her segment: tile_info + region_id

4. as_map_load_objects()             → obj*.ini dosyalari okunur
     Objelerin sektor atamalari yapilir

5. NPC'ler yuklenir
     Her NPC'nin pozisyonuna gore region belirlenir
     region->npcs vektorune eklenir
```

### 4.3 as_map_read_segments — Server Segment Yukleme

```c
// source/server/as_map.c
boolean as_map_read_segments(struct as_map_module * mod)
{
    const char * file_path = ap_config_get(mod->ap_config, "ServerNodePath");
    // node_data dosyasinin tamami belleğe okunur

    uint32_t * cursor = data;
    while (size >= 1032) {
        uint32_t x = *cursor++;           // Sektor X indeksi
        uint32_t z = *cursor++;           // Sektor Z indeksi
        struct as_map_sector * sector = getsector(mod, x, z);

        // 256 segment okunur (z-major sirada dosyadan, [x][z] olarak bellege)
        for (sz = 0; sz < 16; sz++) {
            for (sx = 0; sx < 16; sx++) {
                memcpy(&sector->segments[sx][sz], cursor, 4);
                cursor++;
            }
        }
        size -= 1032;
    }
}
```

### 4.4 Runtime'da Pozisyondan Region Bulma

Server, bir dunya pozisyonundan hangi region'da oldugunu su sekilde belirler:

```c
// source/server/as_map.c
struct as_map_region * as_map_get_region_at(
    struct as_map_module * mod,
    const struct au_pos * pos)
{
    // 1. Pozisyondan segment bul
    struct as_map_segment * s = getsegmentbypos(mod, pos);
    // getsegmentbypos: pos → sektor indeksi → segment indeksi → segments[sx][sz]

    // 2. Segment'in region_id'sini kontrol et
    if (s->region_id > AP_MAP_MAX_REGION_ID)
        return NULL;
    if (!mod->regions[s->region_id].initialized)
        return NULL;

    // 3. Region struct'ini don
    return &mod->regions[s->region_id];
}
```

Bu fonksiyon, O(1) karmasikliginda calisir — dogrudan dizi erisimi.

### 4.5 Karakter Region Degisim Takibi

Server, her karakter hareketi sirasinda region degisimini kontrol eder:

```c
// source/server/as_map.c — karakter hareket callback'i
static boolean cbcharmove(
    struct as_map_module * mod,
    struct ap_character_cb_move * cb)
{
    struct as_map_character * cmap = get_character_data(cb->character);

    // Onceki region
    struct as_map_region * prev_region = cmap->region;

    // Yeni pozisyondaki region
    struct as_map_region * new_region = as_map_get_region_at(mod, &cb->character->pos);

    // Region degisti mi?
    if (prev_region != new_region) {
        cmap->region = new_region;
        onchangeregion(mod, cb->character, prev_region, new_region);
    }
}
```

### 4.6 Region Degisim Etkileri

Bir karakter bir region'dan digerine gectiginde server su islemleri yapar:

```c
static void onchangeregion(
    struct as_map_module * mod,
    struct ap_character * c,
    struct as_map_region * prev,
    struct as_map_region * next)
{
    // Eski region'dan cikis
    if (prev) {
        // Eski regiondaki NPC'leri kaldir (client'a paket gonder)
        for (i = 0; i < vec_count(prev->npcs); i++) {
            struct ap_character * npc = prev->npcs[i];
            as_player_send_custom_packet(mod->as_player, c,
                npc_data->npc_remove_packet, npc_data->npc_remove_packet_len);
        }
    }

    // Yeni region'a giris
    if (next) {
        // Yeni regiondaki NPC'leri goster (client'a paket gonder)
        for (i = 0; i < vec_count(next->npcs); i++) {
            struct ap_character * npc = next->npcs[i];
            as_player_send_custom_packet(mod->as_player, c,
                npc_data->npc_view_packet, npc_data->npc_view_packet_len);
        }

        // Level limiti uygula
        if (next->temp->level_limit > 0) {
            // Karakter seviyesini region limitine dusur
        }
    }
}
```

**Server region degisiminde neler olur:**
- Onceki regiondaki NPC'ler client'tan kaldirilir
- Yeni regiondaki NPC'ler client'a gonderilir
- Region'a ozel seviye limiti uygulanir
- Region ozellikleri (PvP, binek, iksir vb.) degisir

### 4.7 Karakter → Region Baglantisi

```c
// source/server/as_map.h
struct as_map_character {
    struct as_map_sector * sector;        // Karakterin bulundugu sektor
    struct as_map_region * region;        // Karakterin bulundugu region
    uint32_t instance_id;
    // ... NPC paket verileri
};
```

Her karakter (oyuncu veya NPC) bellekte hem sektorune hem region'una referans tutar. Hareket ettikce bu referanslar guncellenir.

### 4.8 Obje Yukleme ve Division Sistemi

```
Obje dosya yolu: content/server/ini/objects/obj{division_id:05d}.ini

Division ID hesaplama:
  division_id = (sektor_x / 16) * 100 + (sektor_z / 16)

Bir division = 16x16 sektor blogu = 256 sektor
```

Bu, server tarafinda objelerin sektorlere atanma birimini gosterir.

---

## 5. Editorde Region Display Sistemi

### 5.1 Region Secimi ve Sektor Taramasi

Kullanici bir region sectiginde asagidaki pipeline calisir:

```
1. Kullanici region secer (UI'dan region_id)
       ↓
2. find_region_sectors(region_id) — node_data dosyasi taranir
       ↓
3. Eslesen segmentlerin centroid'i hesaplanir
       ↓
4. Centroid'e yakin sektorler filtrelenir (3 division mesafe)
       ↓
5. GPU segment mask texture olusturulur
       ↓
6. Kamera centroid'e konumlandirilir
       ↓
7. Gorunur sektorler yuklenir
       ↓
8. Her sektor yuklendiginde .amf'den segment verisi okunur
       ↓
9. rebuild_segment_mask() — GPU mask .amf verisinden yeniden olusturulur
```

### 5.2 find_region_sectors Algoritmasi

Bu fonksiyon `node_data` dosyasini tarar (`source/editor/main.cpp`):

**Adim 1 — Centroid hesaplama:**
```
Tum sektorlerdeki tum segmentler taranir.
Eslesen her segment icin dunya pozisyonu hesaplanir:
  pos_x = WORLD_START_X + sektor_x * SECTOR_WIDTH + (segment_index % 16) * STEPSIZE + STEPSIZE/2
  pos_z = WORLD_START_Z + sektor_z * SECTOR_HEIGHT + (segment_index / 16) * STEPSIZE + STEPSIZE/2
Centroid = ortalama(tum eslesen segment pozisyonlari)
```

**Adim 2 — Yakin sektor filtreleme:**
```
max_mesafe = SECTOR_WIDTH * SECTOR_DEFAULT_DEPTH * 3.0
  = 6400 * 16 * 3 = 307,200 birim

Centroid'den > max_mesafe uzakliktaki sektorler elenir.
Kalan sektorlerden grid sinirlari belirlenir:
  [smin_x, smax_x] x [smin_z, smax_z]
```

**Adim 3 — Boolean sektor gridi:**
```
grid_genislik = smax_x - smin_x + 1
grid_yukseklik = smax_z - smin_z + 1
boolean grid[grid_yukseklik * grid_genislik]
Eslesen segment iceren sektorler TRUE olarak isaretlenir.
```

**Adim 4 — Per-segment GPU mask:**
```
tex_boyut = max(grid_genislik, grid_yukseklik) * 16  (kare texture)
mask[tex_boyut * tex_boyut] — uint8 dizisi, baslangicta 0

Eslesen her segment icin:
  mask_x = (sektor_x - smin_x) * 16 + segment_x
  mask_z = (sektor_z - smin_z) * 16 + segment_z
  mask[mask_z * tex_boyut + mask_x] = 255

Mask parametreleri:
  begin_x = WORLD_START_X + smin_x * SECTOR_WIDTH
  begin_z = WORLD_START_Z + smin_z * SECTOR_HEIGHT
  length = max(grid_genislik * SECTOR_WIDTH, grid_yukseklik * SECTOR_HEIGHT)
```

### 5.3 GPU Segment Mask

Mask texture, fragment shader'da her pikselin region'a ait olup olmadigini kontrol eder.

**Texture ozellikleri:**
- Format: `R8` (tek kanal, 8-bit)
- Boyut: Kare (en genis eksen * 16)
- Sampling: Point (interpolasyonsuz) + Clamp
- Deger: 0 = region disinda, 255 = region icinde

**Fragment shader'da kullanim (`content/shaders/ac_terrain_main.fs.sc`):**

```glsl
uniform vec4 u_maskParams;  // [begin_x, begin_z, length, enabled_flag]
SAMPLER2D(s_texMask, 5);    // Sampler slot 5

void main()
{
    if (u_maskParams.w > 0.5) {           // Mask aktif mi?
        float mu = (world_x - u_maskParams.x) / u_maskParams.z;  // UV-X
        float mv = (world_z - u_maskParams.y) / u_maskParams.z;  // UV-Z
        if (texture2D(s_texMask, vec2(mu, mv)).r < 0.5)
            discard;   // Region disindaki fragment'i at
    }
    // ... normal render devam eder
}
```

**UV donusum formulu:**
```
texture_u = (dunya_x - mask_begin_x) / mask_length
texture_v = (dunya_z - mask_begin_z) / mask_length
```

### 5.4 rebuild_segment_mask — Yuklenen Veriden Mask Yeniden Olusturma

Sektorler yuklendikten sonra, GPU mask'i compact segment dosyalarindan (`.amf`) yeniden olusturulur. Bu, `node_data` ile `.amf` dosyalari arasindaki olasi farkliliklar icin gereklidir — editor segmentleri degistirip kaydettiginde `.amf` guncellenir, ama `node_data` degismez.

```
Adimlar:
1. Yuklenmis tum sektorlerin index sinirlarini bul [smin_x..smax_x, smin_z..smax_z]
2. tex_boyut = max(genislik, yukseklik) * 16
3. Tum sektorleri tara:
   - Her sektorun segment_info->segments[x][z] kontrol et
   - region_id == active_region_id ise mask'te 255 yaz
4. Dunya baslangic ve uzunluk degerlerini hesapla
5. GPU texture'a yukle (ac_terrain_set_segment_mask)
```

### 5.5 CPU Tarafinda Region Kontrolu

GPU mask'in yaninda, CPU tarafinda iki farkli kontrol fonksiyonu kullanilir:

#### is_material_in_region (kapsayici — highlight icin)
```
Bir material'in herhangi bir ucgeninin herhangi bir vertex'i
active_region segmentinde mi?

Her ucgen icin:
  Her vertex icin:
    vertex pozisyonundan segment indeksini hesapla
    segment.region_id == active_region_id ise → TRUE don

Kullanim: Texture highlight render. Shader'daki GPU mask
zaten kesin clipping yapar, bu kontrol sadece submit karari icindir.
```

#### is_material_visible_in_region (kati — texture listesi icin)
```
Bir material'in herhangi bir ucgeninin centroid'i
active_region segmentinde mi?

Her ucgen icin:
  Centroid = (v0 + v1 + v2) / 3
  Centroid pozisyonundan segment indeksini hesapla
  segment.region_id == active_region_id ise → TRUE don

Kullanim: Texture browser listesi. Centroid kontrolu, sinir ucgenlerini
filtreleyerek sadece gercekten gorunen texture'lari listeler.
```

### 5.6 Region Texture Listesinin Hesaplanmasi

Editor, secili region icin kullanilan tum unique texture'lari "Region Textures" penceresinde listeler. Bu liste, export sirasinda region'a ait texture dosyalarini belirlemek icin de kullanilacaktir.

**Hesaplama fonksiyonu:** `browserregiontextures()` — `source/editor/ae_terrain.cpp`

**Tam algoritma:**

```
1. region_textures listesini temizle (vec_clear)

2. Her cached sektor icin:                    // cached_sectors[] — yuklenmis sektorler
     Sektorun detail geometry'si yuklenmis mi kontrol et
       (flags & AC_TERRAIN_SECTOR_DETAIL_IS_LOADED)
     EGER yuklenmemisse → atla

3.   Sektorun geometry linked list'ini tara:
       g = sector->geometry
       WHILE g != NULL:

4.     Her material icin (mi = 0 .. g->material_count-1):

5.       is_material_visible_in_region(sector, g, mi, active_region_id) cagir
           Bu fonksiyon, material'in herhangi bir ucgeninin centroid'i
           active region segmentinde mi kontrol eder.
           FALSE donerse → bu material'i atla

6.       Material'in texture slotlarini tara (ti = 0 .. 4):
           // NOT: 5 slot taranir (0-4), slot 5 taranmaz
           tex = g->materials[mi].tex_handle[ti]
           EGER tex gecersiz (BGFX_INVALID_HANDLE) → atla

7.       Tekrarlik kontrolu:
           region_textures listesinde zaten var mi? (handle.idx karsilastirmasi)
           Varsa → atla
           Yoksa → listeye ekle

8.     g = g->next  (sonraki geometry node)
```

**Onemli detaylar:**

- **Centroid-based filtreleme:** Texture listesi `is_material_visible_in_region` kullanir (centroid bazli, kati). Bu, region sinirinda olup gercekte gorunmeyen texture'larin listelenmesini engeller.

- **5 slot taranir:** `tex_handle[0]` ile `tex_handle[4]` arasi taranir. Slot 5 taranmaz cunku terrain material'lerinde kullanilmaz (segment mask icin ayrilmis).

- **Handle bazli tekrarlik:** Ayni texture dosyasi farkli material'lerde kullanilsa bile, texture yukleme sistemi cache'lemeden dolayi ayni `bgfx_texture_handle_t` doner. Bu yuzden `handle.idx` karsilastirmasi yeterlidir.

- **Sadece yuklenmis sektorler:** Liste sadece `cached_sectors` uzerinden hesaplanir. Bunlar, region secildiginde yuklenen ve gorunur olan sektorlerdir.

- **Her frame yeniden hesaplanir:** `browserregiontextures()` her frame cagrildiginda listeyi sifirdan olusturur.

**Texture handle'dan dosya adina donusum (export icin):**

Export sirasinda texture handle'larindan dosya adlarini elde etmek icin:

```c
char name[64];
ac_texture_get_name(mod->ac_texture, tex_handle, TRUE, name, sizeof(name));
// TRUE = uzantiyi cikar (sadece isim, ornegin "a091300t")
// FALSE = uzanti dahil (ornegin "a091300t.dds")
```

Bu fonksiyon texture modülünün dahili cache'inden handle → dosya adi eslestirmesini yapar.

**Mevcut export'ta texture isimlerinin kaydedilmesi:**

Export fonksiyonu (`export_texture_groups`) ayni `ac_texture_get_name` fonksiyonunu kullanarak texture handle'lardan isimleri alir ve JSON'a yazar:

```json
{
    "groups": [
        {
            "name": "Cim",
            "textures": ["a091300t", "a090200t", "a090100t"]
        }
    ]
}
```

**Texture dosyalarinin fiziksel konumu:**

Texture dosyalari `{ClientDir}/texture/` altinda DDS/TX1/PNG formatlarinda saklanir. Export sirasinda texture isimlerinden dosya yollarini olusturmak icin:

```
texture_path = {ClientDir}/{dat_directory}/{texture_name}.{ext}
```

Texture yukleme sistemi sirasi ile `tx1`, `dds`, `png` uzantilarini dener.

---

## 6. Terrain Mesh Yapisi

### Geometry Yapisi

Her sektorun bir veya birden fazla geometry node'u vardir (linked list):

```c
struct ac_mesh_geometry {
    // Vertex verisi
    uint32_t vertex_count;
    struct ac_mesh_vertex * vertices;     // Pozisyon + Normal + TexCoords
    RwRGBA * vertex_colors;               // Vertex renkleri (RGBA)

    // Ucgen verisi
    uint32_t triangle_count;
    struct ac_mesh_triangle * triangles;   // Indeksler + material_index

    // GPU bufferlari
    uint32_t index_count;
    uint16_t * indices;                    // Index buffer verisi
    bgfx_vertex_buffer_handle_t vertex_buffer;
    bgfx_index_buffer_handle_t index_buffer;

    // Material split'leri
    uint32_t split_count;
    struct ac_mesh_split * splits;
    uint32_t material_count;
    struct ac_mesh_material * materials;

    // Bounding sphere
    RwSphere bsphere;

    // Linked list
    struct ac_mesh_geometry * next;
};
```

### Vertex Yapisi

```c
struct ac_mesh_vertex {
    float position[3];     // Dunya pozisyonu (X, Y, Z)
    float normal[3];       // Normal vektoru
    float texcoord[8][2];  // 8 texture koordinat seti (UV)
};

// Terrain icin kullanilan texcoord'lar:
//   texcoord[0] = Base texture UV
//   texcoord[1] = Layer 0 alpha UV
//   texcoord[2] = Layer 0 color UV
//   texcoord[3] = Layer 1 alpha UV
//   texcoord[4] = Layer 1 color UV
//   texcoord[5-7] = kullanilmaz (terrain icin)
```

### Ucgen Yapisi

```c
struct ac_mesh_triangle {
    uint16_t indices[3];       // 3 vertex indeksi
    uint32_t material_index;   // Bu ucgenin material'i
};
```

### Material Yapisi (Multi-Texture)

```c
struct ac_mesh_material {
    char tex_name[6][64];                // 6 texture adi
    bgfx_texture_handle_t tex_handle[6]; // 6 texture handle

    // Slot yapisi:
    //   [0] = Base texture (ana zemin texture'u)
    //   [1] = Layer 0 alpha (saydamlik maskesi)
    //   [2] = Layer 0 color (katman renk texture'u)
    //   [3] = Layer 1 alpha
    //   [4] = Layer 1 color
    //   [5] = (kullanilmaz veya ozel)
};
```

### Split Yapisi

Split'ler, ayni material'i kullanan ucgenleri gruplayarak GPU draw call'larini optimize eder:

```c
struct ac_mesh_split {
    uint32_t index_count;      // Bu split'teki indeks sayisi
    uint32_t index_offset;     // Index buffer icindeki offset
    uint32_t material_index;   // Kullanilan material indeksi
};
```

**Iliski:**
```
Sector
  └── Geometry
        ├── vertices[]          — tum vertex'ler
        ├── triangles[]         — tum ucgenler (her biri bir material'e ait)
        ├── indices[]           — GPU index buffer (split sirasinda)
        ├── materials[]         — tum material'ler
        └── splits[]            — material basina bir split
              └── Her split: [index_offset, index_count, material_index]
                    → indices[offset..offset+count] → vertices[] → cizim
```

---

## 7. Terrain Render Pipeline

### Normal Render (ac_terrain_render)

```
Her gorunur sektor icin:
  1. Geometry sec (detail veya rough, mesafeye gore)
  2. Model matrix = identity (terrain vertex'leri dunya koordinatlarinda)
  3. Vertex buffer bagla (tum vertex'ler)
  4. Her split icin:
     a. Index buffer bagla (split->index_offset, split->index_count)
     b. Material'in 6 texture'unu sampler 0-4'e bagla (slot 5 mask icin)
     c. Segment mask texture'u sampler 5'e bagla
     d. u_maskParams uniform'u ayarla
     e. bgfx_submit → GPU'ya gonder
```

### Vertex Shader (content/shaders/ac_terrain_main.vs.sc)

```glsl
void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));

    // Texture koordinatlarini aktar
    v_texcoord0 = a_texcoord0;  // Base UV
    v_texcoord1 = a_texcoord1;  // Layer 0 alpha UV
    v_texcoord2 = a_texcoord2;  // Layer 0 color UV
    v_texcoord3 = a_texcoord3;  // Layer 1 alpha UV
    v_texcoord4 = a_texcoord4;  // Layer 1 color UV

    // Dunya XZ pozisyonunu mask UV hesaplamasi icin aktar
    v_texcoord5 = vec2(a_position.x, a_position.z);

    // Basit aydinlatma
    v_color0 = do_light(normal.xyz);
}
```

### Fragment Shader (content/shaders/ac_terrain_main.fs.sc)

```glsl
void main()
{
    // 1. Region mask kontrolu
    if (u_maskParams.w > 0.5) {
        float mu = (v_texcoord5.x - u_maskParams.x) / u_maskParams.z;
        float mv = (v_texcoord5.y - u_maskParams.y) / u_maskParams.z;
        if (texture2D(s_texMask, vec2(mu, mv)).r < 0.5)
            discard;
    }

    // 2. Multi-layer texture blending
    vec4 c = texture2D(s_texBase, v_texcoord0);              // Base
    vec4 l0 = vec4(
        texture2D(s_texColor0, v_texcoord2).rgb,             // Layer 0 color
        texture2D(s_texAlpha0, v_texcoord1).a);              // Layer 0 alpha
    vec4 l1 = vec4(
        texture2D(s_texColor1, v_texcoord4).rgb,             // Layer 1 color
        texture2D(s_texAlpha1, v_texcoord3).a);              // Layer 1 alpha

    c = vec4(l0.rgb * l0.a + c.rgb * (1.0 - l0.a), 1.0);   // Layer 0 blend
    c = vec4(l1.rgb * l1.a + c.rgb * (1.0 - l1.a), 1.0);   // Layer 1 blend

    gl_FragColor = vec4(saturate(c.xyz * v_color0.xyz), 1.0);
}
```

---

## 8. Region Export Stratejisi

### 8.1 Genel Yaklasim

Region export, editordeki display mantigi ile ayni temel adimlari kullanir:

```
Display Pipeline:              Export Pipeline:
─────────────────              ────────────────
1. Region sec                  1. Region sec
2. Sektorleri tara             2. Sektorleri tara (ayni algoritma)
3. Segment mask olustur        3. Ucgenleri filtrele
4. GPU'da fragment discard     4. Filtrelenmis mesh'i dosyaya yaz
5. Goruntuyu goster            5. Texture'lari kopyala
                               6. Material mapping yaz
```

Temel fark: Display'de GPU her fragment'i mask'e gore atar; export'ta ise CPU tarafinda ucgenleri kesip yeni mesh olusturmak gerekir.

### 8.2 Export Edilecek Veriler

Her region icin asagidaki veriler export edilmeli:

#### A. Mesh Verisi
```
Her region icin:
  - vertices[]: pozisyon, normal, UV koordinatlari
  - triangles[]: vertex indeksleri + material referansi
  - Her material/split icin ayri mesh parcasi
```

#### B. Heightmap (Opsiyonel)
```
Region sinirlarindaki segment gridi uzerinden heightmap:
  - Vertex Y degerleri kullanilarak grid-based yukseklik haritasi
```

#### C. Texture Verisi
```
Her material icin:
  - Base texture dosyasi
  - Layer alpha texture'lari
  - Layer color texture'lari
  - Texture isimleri → dosya yolu eslestirmesi
```

#### D. Metadata
```
  - Region sinirlari (dunya koordinatlari)
  - Sektor listesi
  - Segment haritasi (hangi segment bu region'a ait)
  - Material → texture eslestirmesi
```

### 8.3 Ucgen Filtreleme Algoritmasi

Display'deki `is_material_in_region` / GPU mask yerine, export'ta ucgenleri kesin olarak filtrelemeliyiz:

```
ADIM 1: Region'a ait sektorleri belirle
  find_region_sectors(region_id) ile ayni tarama

ADIM 2: Her sektorun geometry'sini isle
  Her sektor icin:
    Her geometry node icin:  (geometry->next linked list)
      Her ucgen icin:
        centroid = (v0 + v1 + v2) / 3
        segment = get_segment_by_pos(sektor, centroid.x, centroid.z)
        EGER segment.region_id == target_region_id:
          Bu ucgeni export listesine ekle

ADIM 3: Vertex re-indexing
  Export edilen ucgenler orijinal vertex indekslerini kullanir.
  Yeni bir vertex dizisi olusturup indeksleri yeniden map'le:
    - Kullanilan vertex'leri topla (unique set)
    - Yeni 0-baslangicli indeksler ata
    - Ucgen indekslerini guncelle

ADIM 4: Material bazli gruplama
  Ucgenleri material_index'e gore grupla.
  Her grup ayri bir material slot'una karsilik gelir.
```

**Sinir ucgenleri icin iki strateji:**

1. **Centroid-Based (Onerilen):** Ucgenin centroid'i region'da ise dahil et. Temiz sinirlar olusturur, kucuk bosluklar olabilir.

2. **Any-Vertex:** Herhangi bir vertex region'da ise dahil et. Kapsayici, ancak komsu region'larla ucgen cakismasi olusabilir.

### 8.4 Onerilen Export Dizin Yapisi

```
export/
└── terrain/
    ├── texture_layers.json                    (global layer tanimlari)
    └── regions/
        └── {region_id}/
            ├── metadata.json                  (region metadata)
            ├── mesh/
            │   ├── sector_{sx}_{sz}.obj       (veya .fbx)
            │   └── ...
            ├── textures/
            │   ├── a091300t.dds               (kullanilan texture dosyalari)
            │   └── ...
            ├── materials.json                 (material → texture eslestirmesi)
            ├── segment_map.json               (segment region haritasi)
            └── texture_layers.json            (region texture gruplari)
```

### 8.5 Onerilen JSON Formatlari

#### metadata.json

```json
{
    "region_id": 42,
    "region_name": "Archlord_Plains",
    "world_bounds": {
        "min_x": -51200.0,
        "min_z": -44800.0,
        "max_x": -38400.0,
        "max_z": -32000.0
    },
    "sectors": [
        {
            "index_x": 392,
            "index_z": 393,
            "extent_start": [-51200.0, -44800.0],
            "extent_end": [-44800.0, -38400.0],
            "file": "mesh/sector_392_393.obj"
        }
    ],
    "total_vertices": 128456,
    "total_triangles": 85432,
    "material_count": 12,
    "coordinate_system": "left_handed_y_up"
}
```

#### materials.json

```json
{
    "materials": [
        {
            "index": 0,
            "base_texture": "textures/a091300t.dds",
            "layers": [
                {
                    "alpha_texture": "textures/a091300t_alpha0.dds",
                    "color_texture": "textures/a090200t.dds"
                },
                {
                    "alpha_texture": "textures/a091300t_alpha1.dds",
                    "color_texture": "textures/a092100t.dds"
                }
            ]
        }
    ]
}
```

#### segment_map.json

```json
{
    "region_id": 42,
    "segment_size": 400.0,
    "sectors": [
        {
            "index_x": 392,
            "index_z": 393,
            "segments": [
                {"x": 0, "z": 0, "region_id": 42},
                {"x": 0, "z": 1, "region_id": 42},
                {"x": 1, "z": 0, "region_id": 0}
            ]
        }
    ]
}
```

---

## 9. Dikkat Edilecekler

### 9.1 Indeksleme Sirasi

**Kritik:** Segment dizilimi `segments[x][z]` — X birinci indeks, Z ikinci indeks.

```c
// DOGRU:
segment = sector->segment_info->segments[sx][sz];

// YANLIS (eski bug):
segment = sector->segment_info->segments[sz][sx];
```

Bu hata daha once display sisteminde tespit edilip duzeltildi. Export kodunda da ayni dikkat gerekli.

### 9.2 Dosya Formati ve Bellek Sirasi Farki

Segment dosyalari (`.amf` ve `node_data`) z-major sirada yazilir, ancak bellekte `[x][z]` olarak saklanir:

```
Dosya okuma:  buf[z * 16 + x] → segments[x][z]  (transpose gerekli)
Dosya yazma:  segments[sx][sz] → sirayla yaz (ic dongu x, dis dongu z)
```

### 9.3 Region ID Maskeleme

Eski dosya formatlarinda (v1, v2) ve node_data'da region_id alt 8 bit'e maskelenir:

```c
region_id = raw_value & 0x00FF;
```

Yeni compact formatta (v3, v4) raw struct olarak okunur, maskeleme yapilmaz.

### 9.4 Multi-Texture Material Yapisi

Terrain material'leri 3 katmana kadar destekler:

```
Slot 0: Base texture         ─── her zaman var
Slot 1: Layer 0 alpha mask   ─┐
Slot 2: Layer 0 color        ─┘ opsiyonel cift
Slot 3: Layer 1 alpha mask   ─┐
Slot 4: Layer 1 color        ─┘ opsiyonel cift
Slot 5: (kullanilmaz)

Blending formulu (shader'da tanimli):
  result = layer0_color * layer0_alpha + base * (1 - layer0_alpha)
  result = layer1_color * layer1_alpha + result * (1 - layer1_alpha)
```

### 9.5 Vertex Pozisyonlari Dunya Koordinatlarinda

Terrain vertex'leri model matrix = identity ile render edilir. Yani `vertex.position` dogrudan dunya koordinatidir.

### 9.6 Sektor Sinirlari ve Geometry Linking

Bir sektor birden fazla geometry node'una sahip olabilir (`geometry->next`). Export sirasinda linked list'in tamami taranmalidir:

```c
struct ac_mesh_geometry * g = sector->geometry;
while (g) {
    // g'deki ucgenleri isle
    g = g->next;
}
```

### 9.7 Rough vs Detail Geometry

Her sektorun iki geometrisi vardir:
- `sector->geometry` — detail (yuksek cozunurluk)
- `sector->rough_geometry` — dusuk cozunurluk (uzak render icin)

Export icin **her zaman detail geometry** kullanilmalidir.

### 9.8 node_data vs .amf Tutarsizligi

`node_data` dosyasi server tarafinda kullanilan ana kaynak. `.amf` dosyalari ise editorden kaydedilmis guncel client verisi. Editorde segment region_id degistirilip kaydedildiginde `.amf` guncellenir ama `node_data` degismez. Export sirasinda hangi kaynagin kullanilacagina karar verilmeli:

- `.amf` → editordeki son degisiklikleri yansitir
- `node_data` → server'in bildigi orijinal veriyi yansitir

---

## 10. Mevcut Export Sistemi

Projede su anda sadece texture layer metadata'si export ediliyor:

### Global Layer Tanimlari
```
Yol: export/terrain/texture_layers.json
Icerik: { "layers": ["Cim", "Daglik", "Parke", "Toprak"] }
```

### Region-Spesifik Layer Atamalari
```
Yol: export/terrain/{region_id}/texture_layers.json
Icerik: {
    "groups": [
        { "name": "Cim", "textures": ["a091300t", "a090200t", ...] },
        { "name": "Daglik", "textures": ["a090800t", ...] }
    ]
}
```

Bu mevcut export, sadece isimleri kaydeder — mesh verisi, vertex pozisyonlari veya tam material bilgisi henuz export edilmiyor.

---

## 11. Ozet: Display → Export Karsilastirmasi

| Islem | Display (GPU) | Export (CPU) |
|-------|---------------|-------------|
| Segment filtresi | R8 mask texture + shader discard | Ucgen centroid → segment lookup |
| Ucgen secimi | Tum ucgenler gonderilir, GPU atar | CPU'da filtrelenir, sadece eslesen yazilir |
| Material eslestirme | Split bazinda submit | Material bazinda gruplama |
| Vertex verisi | GPU buffer'da kalir | Dosyaya yazilir (OBJ/FBX/JSON) |
| Texture verisi | Runtime handle | Dosya kopyalama + isim eslestirmesi |
| Performans | Real-time (60fps) | Batch islem (bir kere calisir) |
| Hassasiyet | Per-fragment (piksel bazinda) | Per-triangle (ucgen bazinda) |

**Ana prensip:** Hem display hem server hem export ayni `segments[x][z].region_id` verisini kullanir. Fark yalnizca isleme yontemidir:
- **Server:** Pozisyon → segment → region_id → oyun kurallari (O(1) lookup)
- **Display:** GPU mask texture → fragment shader discard (per-pixel)
- **Export:** CPU ucgen filtreleme → dosya yazma (per-triangle)
