# godot-rs + gael - spec roboczy

Stan: 2026-10-18, zebrane z braindumpu. Wszystko poniżej to propozycja do poprawiania; sekcja "Do rozstrzygnięcia" to
miejsca, gdzie notatki mówią dwie różne rzeczy.

Rozstrzygnięte 2026-10-18: rdzeń (gael i jądro silnika) w C89 według manifestu; gemy w dowolnym języku (C, Rust, C++,
gael, ...); język ma własne repo Gucixdev/gael, silnik przyjdzie później.

## W jednym akapicie

Silnik i edytor gier w duchu Godota (UI/UX wzorowane na Godocie i GDStudio), ale z inną filozofią: rzadkie, duże
wydania stable i ciągła gałąź testing zamiast częstych małych update'ów. Jądro jest małe, wszystko inne to gem:
wersjonowany pakiet z `requires` / `depends` / `exclude`, który daje nody, komendy, tryby edytora i importery. Skrypty i
sam edytor pisze się we własnym języku gael (s-wyrażenia, statyczne typy, bytecode VM; AOT i JIT jako gemy). Edytor
działa jak Emacs (wszystko jest buforem, trybem, komendą i hookiem, zmienialne na żywo), z natywnym GUI dla
początkujących i pluginem do Emacsa dla zaawansowanych. Docelowo integruje Blendera, GIMP-a, Kritę i resztę na tyle, że
przestają być potrzebne. Najpierw Linux natywnie.

## Kolejność

Z notatek: "najpierw język, potem edytor", "edytor emacslike w samym gaelu, dodawać części po kolei".

1. gael - język, sam w sobie użyteczny (REPL, skrypty, osadzanie).
2. Jądro edytora w gaelu - bufor, okno, tryby, komendy (Emacs bez GUI).
3. Gemy silnika - ECS na tagach, renderer, 2D, fizyka - pierwsza grywalna scena.
4. GUI edytora - układ Godota/GDStudio, inspektor, drzewo sceny, viewport jako bufor 3D.
5. Twoje elementy - GAS, MATS, record/replay/rollback, gemy gameplayowe, integracje narzędzi.

## Do rozstrzygnięcia

1. **Rdzeń w Ruście czy w C - rozstrzygnięte: C89.** Jądro (gael: reader, typy, kompilator, VM; potem bufor i okno)
   jest w C89 według manifestu: areny zamiast malloc, bez rekurencji, unity build, statyczne linkowanie, zero
   zależności, typy z holyc (`o`, `i8`..`i64`, `u8`..`u64`, `f32`, `f64`). Rust zostaje tam, gdzie daje gotowe rzeczy
   (wgpu, rapier, Cranelift) - jako gemy za interfejsem C.
2. **Platformy.** Notatki: Vulkan/OpenGL/Metal/WASM/Android, Linux/Windows - i zaraz potem "na razie Linux native, brak
   potrzeby Windowsa". Propozycja: Linux najpierw; wgpu zostawia otwarte drzwi (Vulkan, Metal, DX12, GL, WebGPU) bez
   pisania kilku backendów.
3. **Typy jako gem a błędy w kompilacji.** Notatki: "może typy też jako gem, a fundament to tylko parser, okno i
   bufory" oraz "najważniejsze, żeby jak najwięcej błędów wychodziło na etapie kompilacji". Kontrola typów w gemie
   oznacza, że bez niego nie ma gwarancji. Propozycja: type checker w jądrze; gemy dokładają nowe typy (struktury,
   enumy, typy silnika) przez API systemu typów, ale go nie zastępują.
4. **requires / depends / exclude.** Propozycja znaczeń: `requires` - musi być, z zakresem wersji, inaczej błąd przy
   ładowaniu projektu; `depends` - ładuje się po nim, jeśli jest (opcjonalne); `exclude` - nie mogą być razem.
5. **Wiele bibliotek na tę samą rzecz.** Lista ma sokol i wgpu, rapier i Bullet i Box2D, flecs i własny ECS. Propozycja:
   jedna domyślna na zadanie, reszta jako alternatywne gemy. Przy rdzeniu w C naturalne domyślne to sokol (C, single
   header) albo wgpu-native (C API do wgpu), własny ECS w C (masz go w holyC), a fizyka jako gem: rapier w Ruście za C
   ABI albo Box2D (C).
6. **Wycinanie gemów z buildu gry.** "Gra sama wycina nieużywane gemy" - propozycja: build gry
   analizuje użyte nody, typy i komendy i pomija gemy, których nic nie używa; ręczne wyłączenie zostaje jako opcja.

## gael - język

Cel z notatek: "coś pomiędzy" Lispem, GDScriptem, bashem, Jai i Perlem (oraz Lua, Teal, Elisp). Statycznie typowany w
większości, wysoka wydajność, błędy na etapie kompilacji, mało boilerplate'u bez strzelania sobie w stopę jak w C++.

### Składnia

S-wyrażenia, typy zapisywane jak w GDScripcie. Szkic, nie decyzja:

```lisp
(defn heal ((e : Entity) (amount : int)) : int
  (let ((hp : int (get e :hp)))
    (set e :hp (min 100 (+ hp amount)))))

(export speed : float 4.5)                 ; jak @export w GDScripcie, leniwie

(query (pos : Position) (vel : Velocity)   ; dostęp do ECS przez makro
  (set pos (+ pos (* vel dt))))

(|> (read-file "level.txt") lines (filter non-empty?) count)   ; pipe
(if (and (alive? e) (has-tag? e :burning)) (damage e 1))     ; && || ze skracaniem
(sh "git status --short")                  ; klej jak w bashu
(println "hp: %d" hp)                      ; printf / println
```

### Jądro języka (kernel) a gemy

- **Jądro:** reader (s-wyrażenia), makra (defmacro + x-macro do tablic opkodów), type checker, kompilator do
  bajtkodu, VM, REPL, alokatory (arena, pool, refcount, persistent arena), FFI do hosta, bufor i okno.
- **Typy w jądrze:** int, float, bool, string, symbol/tag, array, map, option, fn, struct, enum, traits.
- **Z gemów:** deltatime, tick, uuid, wektory i macierze z SIMD, kolekcje specjalne (priority queue, event queue),
  async/await i korutyny/fibery, joby (fine-grained, master/worker), AOT, JIT, kryptografia, obfuskacja.

### Pamięć i wydajność

- Arena per klatka i per system, pool na encje, refcount na współdzielone zasoby (assety), persistent arena (plik
  mmapowany) pod snapshoty i zapis stanu.
- Typy cache-friendly z założenia: tablice struktur jako SoA, wektory jako typy SIMD.
- Wykonanie: najpierw interpreter bajtkodu i REPL; AOT i JIT jako gemy (np. Cranelift w gemie napisanym w Ruście,
  albo własny backend w C).

### Błędy w kompilacji (priorytet z notatek)

Kontrola typów, wyczerpujące `match` na option i enumach, nieużywane zmienne, brakujące `requires` gemów, błędne
odwołania do nodów i komponentów w zapytaniach ECS - wszystko przed uruchomieniem, z miejscem w pliku (linia, kolumna).

## Gemy

- **Wszystko jest gemem**, poza `kernel` i `editor` (wbudowane). Renderer, serwer, ECS, web, UI, 2D, 3D, fizyka - gemy.
- **Gem w dowolnym języku**, trzy rodzaje:
  - *źródłowy* - kod w gaelu, kompilowany razem z projektem;
  - *natywny* - biblioteka w C, Ruście, C++, Zigu, czymkolwiek, która wystawia jedno wejście C
    (`gael_gem_init(const GaelApi *api)`; w C++ `extern "C"`, w Ruście `#[no_mangle] extern "C"`), linkowana
    statycznie do buildu gry albo ładowana dynamicznie w edytorze;
  - *procesowy* - osobny program, który rozmawia z silnikiem przez stdin/stdout albo socket (jak LSP czy MCP); dla
    języków, których nie da się wlinkować (Python, Go z GC) i dla mostów do Blendera czy GIMP-a.
  Typy i funkcje, które gem wystawia, trafiają do type checkera gaela, więc wywołania z gaela są sprawdzane w
  kompilacji niezależnie od języka gemu.
- **Manifest** w s-wyrażeniach, sprawdzany przed uruchomieniem:

  ```lisp
  (gem fsm
    :version "0.3.1"
    :requires ("kernel >= 0.1" "ecs >= 0.2")
    :depends ("debug-draw")
    :exclude ("hfsm-legacy")
    :provides (:nodes (StateMachine) :commands (fsm-graph)))
  ```

- **Nowy projekt:** wybór gemów (2d, 3d, sdf, aabb, noise...). **Ustawienia:** globalne i per projekt, warstwami
  (domyślne gemu, globalne, projekt).
- **Rejestr:** wszystkie gemy na Twoim GitHubie, Ty decydujesz o merge'ach; gałąź `experimental` dla chętnych. Prosty
  menedżer pakietów: indeks w repo, lockfile w projekcie.
- **Kanały wydań:** stable co określony czas z większą liczbą funkcji, testing na bieżąco.

### Katalog gemów z notatek

- **Fundament:** kernel, editor, renderer, server, ecs, web, ui, 2d, 3d, physics, buffer, buffer3d.
- **Rozgrywka:** soulslike, voxel, input buffer, parallax bg, dialogue (dialogi w Markdownie), event flags (flag list,
  flag map), priority queue, event queue, fsm, hfsm, A*, voronoi, perlin, LOD, HLOD, raycast, raytrace, world-env, GAS
  (zdolności i efekty, z Unreala), timeact editor (oś zdarzeń animacji jak TAE w grach FromSoftware), edytor poziomów
  jak w Quake'u (w stylu TrenchBroom).
- **Grafika:** MATS - materiały, które mogą być czymkolwiek (szkło, które można rozbić; olej na ziemi, który można
  podpalić), plus matcap, vertex animation, Phong, Blinn-Phong, POM/SPOM i inne stare techniki.
- **Runtime:** joby, korutyny, fibery, AOT, JIT, kryptografia, obfuskacja, record / replay / rollback / snapshot.
- **Edytor jak Emacs:** magit, evil, org, mail, todo, dired, shell, tryb gamedev, gry terminalowe, dystrybucja gry,
  zarządzanie assetami (wersjonowanie lokalne i globalne, wzorowane na Lore od Epic Games: magazyn adresowany treścią,
  częściowe checkouty), AI/LLM od początku (Claude Code, edytor w stylu Cursora, Ollama), hooki.
- **Narzędzia:** Blender, Krita, GIMP, Inkscape, OpenSCAD, Audacity, REAPER, FL Studio, TrenchBroom - najpierw jako
  mosty (tak jak dziś masz MCP do Godota i Blendera), potem własne gemy tam, gdzie mają zastąpić program.

## Edytor

- **Dwie twarze:** natywne GUI dla początkujących (układ Godota/GDStudio: drzewo sceny, inspektor, system plików,
  viewport, zakładki) i plugin do GNU Emacsa dla zaawansowanych - tego Godot nie ma.
- **W środku jak Emacs:** bufor tekstowy (podrasowany) i bufor 3D (viewport) jako gemy, tryby główne i poboczne,
  komendy, keymapy, hooki; wszystko w gaelu, zmienialne na żywo. Przyciski i panele osadzane w buforach.
- **UI:** layout na constraintach Cassowary z procentami (skalowanie ekranu), animacje sprężyną z tłumieniem (w stylu
  Apple), logika UI oddzielona od krytycznej.

## Silnik

- **ECS oparty na tagach**, zapytania przez makra gaela; joby w stylu DOTS z Unity; GAS z Unreala.
- **Deterministyczna symulacja:** record / replay / rollback / snapshot (zapis zdarzeń, persistent arena).
- **Domyślne biblioteki:** wgpu (renderer), rapier (fizyka 2D/3D), assimp (import modeli), ffmpeg i ImageMagick
  (media), GNU FreeFont i fonty OFL. Coś jak LÖVE: silnik, którego da się użyć jako zwykłej biblioteki.

## Co już masz i da się wziąć

- **lazyui/tui (C):** solver Cassowary, sprężyny RK4, renderer różnicowy, motywy - gotowa podstawa layoutu i animacji
  edytora.
- **ECS w holyC (C):** archetypy, tagi, widoki, iteracja przyjazna SIMD - wzór dla gemu ecs.
- **ashparser / ashtree (Mojo):** kombinatory parserów i płaskie AST w arenie (CTree) - wzór parsera i AST gaela.
- **frost (C):** multi-cursor, tryby vima, sesje - wzór bufora.
- **progit (Rust):** git TUI w stylu magita - zaczyn gemu magit.
- **Godot toolbox:** jednonodowe addony (SaveSystem, AnimPlayer, DialogueSystem, Control3D) - pierwsi kandydaci na gemy.
- **emacs-mcp i claude-mcp.el:** wzór protokołu dla pluginu do Emacsa.

## Pierwszy kamień milowy: gael 0.1

Coś, co działa samo i da się wydać, nawet zanim powstanie silnik:

- reader s-wyrażeń z pozycjami w pliku i czytelnymi błędami,
- `let`, `defn`, `if`, `when`, `loop`, `match` na prostych typach, `struct`, `option`,
- type checker dla tego podzbioru (błąd przed uruchomieniem, z linią i kolumną),
- interpreter, potem kompilator do bajtkodu i VM z areną,
- REPL, `println` / `printf`, `sh` (uruchomienie programu, wynik jako string),
- testy: każdy przykład z `examples/` uruchamiany w CI, każdy błąd typu ma test, który go wywołuje,
- jedna statyczna binarka `gael` na Linuxa, README z przykładami.

Potem: makra i moduły (0.2), manifest i resolver gemów (0.3), bufor i okno (0.4, początek edytora).

### Stan (2026-10-19)

- **0.1:** reader, `def`/`defn`/`let`/`set`, `if`/`when`/`while`/`do`, `and`/`or`/`not`, int/float/bool/string,
  type checker zgłaszający wszystkie błędy naraz, bajtkod i VM, REPL, `print`/`println` z kodami sprawdzanymi w
  kompilacji, `sh` (na razie kod wyjścia), statyczna binarka, testy na gcc, clang i sanitizerach. Zamiast `loop` jest
  `while`.
- **0.2:** `struct` (wartości bez alokacji, pola przez kropkę: `b.pos.x`, konstruktor pozycyjny albo z nazwami pól),
  `enum`, `(option T)` z `some`/`none` (typ `none` wynika z miejsca użycia), `match` z kontrolą wyczerpania i
  martwych ramion, `%v` dla każdej wartości. Kompilator liczy największą głębokość stosu operandów każdej funkcji, a VM
  sprawdza przed wejściem, że cała ramka się mieści (wcześniej stały zapas 256 komórek dało się przekroczyć). Stringi
  z czasu wykonania w jednej arenie tekstu: `format`, `sh` zwracające wyjście komendy, `system` z kodem wyjścia,
  `parse-int` i `parse-float` zwracające `(option ...)`.
- **CI:** GitHub Actions uruchamia shellcheck i `./test` przy każdym pushu.
- **Następne:** tablice, operacje na stringach, potem makra i moduły, manifest gemów, bufor i okno.
  Numery wersji przesuwają się o jeden względem planu wyżej.

## Ryzyka, uczciwie

- **Skala:** własny język, edytor, silnik, system gemów i zamienniki Blendera to lata pracy jednej osoby. Każdy etap
  musi być użyteczny sam w sobie, żeby coś wyszło na zewnątrz, zanim powstanie całość.
- **Ekosystem:** Godot ma ponad dekadę i tysiące addonów. Wygrać można skupieniem (Emacs, gemy, Twój workflow,
  wydania stable), nie równaniem funkcji.
- **Kod pisany przez AI:** rdzeń gaela (VM, type checker) musisz rozumieć sam - przy utrzymaniu i na rozmowach.
