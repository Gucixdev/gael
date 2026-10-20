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

S-wyrażenia, typy zapisywane jak w GDScripcie. To, co już działa, opisuje README; poniżej szkic części z gemów:

```lisp
(defn heal ((e : ECSEntity) (amount : int)) : int   ; typy gemu ecs mają prefiks ECS
  (let ((hp : int (ecs-get e :hp)))
    (ecs-set e :hp (min 100 (+ hp amount)))))

(export speed : float 4.5)                          ; jak @export w GDScripcie, leniwie

(ecs-query (pos : Position) (vel : Velocity)        ; zapytanie do ECS przez makro gemu ecs;
  (set pos (+ pos (* vel dt))))                     ; Position i Velocity to struktury gry
(db-query "select name from saves")                 ; inny rodzaj zapytania, inny prefiks

(|> (sh "cat level.txt") (split "\n") len)          ; pipe - działa już dziś
(if (&& (alive? e) (ecs-has-tag? e :burning)) (damage e 1))
(&& "make" "make install")                          ; && jak w bashu: druga komenda po sukcesie pierwszej
(println "hp: %d" hp)
```

### Nazwy

- Typy i formy gemu mają prefiks swojego systemu: gem ecs daje `ECSEntity`, `ECSWorld`, `ECSComponent`,
  `(ecs-query ...)`, `(ecs-get e :hp)`; gem bazy danych - `DBQuery`, `(db-query ...)`. Po nazwie widać, skąd coś
  pochodzi, i dwa gemy nie kłócą się o `query`.
- Rdzeń jest jeden, więc jego nazwy są krótkie: `vec`, `map`, `push`, `get`, `split`.
- Struktury gry nazywa gra (`Position`, `Velocity`); prefiks dotyczy tego, co daje gem.

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

- **Kształt gemu (decyzja 2026-10-20):** każdy gem to osobna rzecz. Gem z jednego pliku to jeden plik
  (`nazwa.gael`); gem z więcej niż jednego pliku to folder (`nazwa/`, wejście `nazwa/main.gael`, manifest obok).
- **Folder gaela (decyzja 2026-10-20),** jak `~/.emacs.d`: `$GAEL_HOME` albo `~/.gael`. W nim tylko kilka plików
  (`main.gael` - jak `init.el`, `repldump.gael` - zapis REPL-a, później ustawienia), a gemy jako pliki albo foldery
  według zasady wyżej (`gems/`). Już działa: REPL na starcie uruchamia `main.gael`, a każdą formę, która przeszła bez
  błędu, dopisuje do `repldump.gael` pod linią z datą i katalogiem sesji (odtworzenie: `gael < ~/.gael/repldump.gael`,
  bo REPL pozwala na ponowne definicje). `GAEL_HOME=` (pusty) wyłącza jedno i drugie; `./test` używa folderu
  tymczasowego.
- **Ładowanie (decyzja 2026-10-20):** liczba zainstalowanych gemów nie wpływa na start. Start czyta tylko
  `main.gael`, a ten wymienia gemy włączone; gemu wyłączonego nikt nie otwiera. Gem albo tryb włącza się i wyłącza
  w trakcie działania: włączenie kompiluje gem do działającego programu (tak jak REPL dokłada każdą formę), wyłączenie
  zdejmuje jego komendy, keymapy i hooki, a jego dane zwalnia refcount. Skompilowany kod zostaje w pamięci do
  restartu; prawdziwe wyładowanie kodu i cache skompilowanego kodu przyjdą, gdy pomiar pokaże potrzebę. Powód: edytor
  ma z czasem zastąpić Blendera, GIMP-a i Kritę, więc trybów będzie dużo i będą duże - start ma kosztować tyle, co
  tryby włączone. Większość gemów i tak włącza się w trakcie pracy, więc ładowanie w działającym programie to główna
  droga, nie wyjątek. Pomiar (gcc -O2, mediana 7 przebiegów `gael -c`): plik 6000 linii (1000 funkcji ze strukturą,
  `let`, `match`, `format`) kompiluje się w ~14 ms ponad ~3 ms startu procesu - mniej niż klatka przy 60 fps, więc
  cache jeszcze niepotrzebny. Ten plik to ~46 tys. instrukcji, a tablica kodu ma dziś 2^18 (razem ~34 tys. linii
  gaela), funkcji 4096: przed ładowaniem gemów w trakcie te tablice przejdą na rezerwację przestrzeni adresowej jak
  sterta. Zrobione: węzły, kod, funkcje, typy, nazwy i makra są rezerwacjami (strony dopiero przy zapisie), limity
  2^24 instrukcji, 65536 funkcji, globalnych i typów, 2^21 węzłów na plik; BSS binarki spadł z 53 MB do 3,4 MB, start
  ~1,6 ms (`/bin/true` ~1,4 ms). Plik z 30 tys. funkcji (390 tys. instrukcji) kompiluje się i działa w ~84 ms. Indeks
  nazw zaczyna od 4096 miejsc i rośnie dwukrotnie: rozrzucony po 8 MB kosztowałby błąd strony na każdą nazwę.
- **Start (decyzja 2026-10-20):** program ma się odpalać błyskawicznie, w ułamku sekundy. Dziś `gael -e` startuje w
  ~1,5 ms, tyle co `/bin/true` w tym samym pomiarze (`python3 -c pass`: ~15 ms; 200 uruchomień, statyczna binarka,
  rezerwacje mmap nic nie kosztują, dopóki nic nie jest zapisane). Koszt rośnie tylko z gemami włączonymi na starcie:
  ~2,5 ms kompilacji na 1000 linii. Gdy to przekroczy kilkadziesiąt milisekund, przyjdzie obraz skompilowanego stanu
  (jak pdumper w Emacsie): zapisany po kompilacji, przy starcie mapowany z pliku, więc czas nie zależy od rozmiaru.
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
- **0.3:** `(vec T)` - uchwyt do płaskiego bloku komórek na stercie (rośnie przez podwajanie, elementy-struktury leżą
  ciągiem), `push`/`get`/`put`/`pop`/`len`, `get` i `put` sprawdzają indeks; `for` po wektorze i po zakresie;
  struktura może zawierać `(vec Self)`, więc drzewa (scena, AST) mają skończony rozmiar. Stringi są widokami (przesunięcie
  i długość w arenie tekstu): `substr`, `split`, `trim` bez kopiowania, do tego `byte`, `find`, `join`, `char`.
  Wbudowane funkcje o stałej sygnaturze są w jednej tabeli w kompilatorze. `./test` uruchamia też przykłady z README.
  Wejście i wyjście: `read-file`, `write-file`, `read-line`, `args` (`gael PLIK ARGUMENTY...`), `getenv`, `exit`,
  `eprint`/`eprintln` na stderr.
- **Moduły:** `(use "plik.gael")` - ścieżka względem pliku, każdy plik raz (kanoniczna ścieżka), cykl to błąd w
  miejscu `use`, najpierw kompilacja wszystkich plików, uruchomienie dopiero bez błędów. Wspólna przestrzeń nazw;
  przestrzenie nazw gemów przyjdą z manifestem.
- **Pola wyrażeń:** `(get v 0).pos.x` - czytnik zamienia `.pole` tuż po `)` na `(. wyrażenie pole)`.
- **Mapy:** `(map K V)` z kluczem int/bool/string/enum, kolejność wstawiania zachowana, otwarte adresowanie na
  stercie; `insert`, `lookup` i `remove` (opcje), `has`, `keys`, `values`, `len`, `for` po kluczach.
- **Refcount (zrobiony 2026-10-19).** Licznik właścicieli mają stringi z czasu wykonania, `vec` i `map`. Kompilator
  zna typy, więc sam wstawia liczenie: odczyt zmiennej to kopia (`RETAIN`), wyrzucona wartość i koniec zakresu
  zmiennej oddają swoje (`DROP`, `RELEASE`), instrukcja zużywa to, co bierze. Odczyt, który tylko patrzy (`len`,
  `get`, `=`, `lookup`, ukryty wektor pętli `for`), pożycza zmienną bez liczenia, jeśli dalsze argumenty nie mogą
  jej zmienić (lokalnej: nie ma w nich `set` tej nazwy; globalnej: same atomy). Obiekt z licznikiem 0 jest zwalniany
  od razu, a to, co trzymał, idzie na ten sam łańcuch, więc zwalnianie nie jest rekurencyjne. Sterta to buddy
  allocator na jednej statycznej tablicy (bloki 2^k, łączone z wolnym sąsiadem); rosnący `vec` podwaja się w miejscu,
  gdy blok za nim jest wolny. Stringi programu (stałe) leżą w arenie tekstu z bitem 63 i nie są liczone; `substr`,
  `split` i `trim` kopiują ze stringów z czasu wykonania. Cykl (węzeł we własnej liście dzieci) nie zostanie
  zwolniony: `GAEL_CHECK=1` zgłasza go na końcu programu i zamazuje zwolnioną pamięć, więc nieaktualny odczyt widać w
  wyniku. `./test` uruchamia wszystko z `GAEL_CHECK=1`. Pomiar (gcc -O2, 5 przebiegów): pętle po wektorze 2M intów
  ~565 ms wobec ~575 ms przed refcountem, `fib 30` ~54 ms wobec ~60 ms, 200k `format` z mapą ~45 ms wobec ~37 ms
  (przydział i zwolnienie zamiast samego przyrostu areny). Poprawiony przy okazji błąd: `(option Self)` w polu
  `(vec ...)` struktury miało rozmiar policzony, zanim struktura go znała.
- **Pamięć - podział (decyzja 2026-10-19).** Refcount dla tego, co długo działa i zmienia się w trakcie: edytor, jego
  gemy i tryby (bufory: wektor, pixel art, raster, rysunek techniczny, rzeźba 3D, tekst), serwer, backend strony.
  Arena i pool dla wydanych gier: `pool` (stała pojemność, SoA, uchwyty z generacją) na encje, komponenty i
  cząsteczki; `arena` na pamięć klatki, poziomu albo jednego żądania serwera, zwalnianą naraz. Ten sam kod w dwóch
  trybach: w edytorze obiekty z areny też są liczone, a koniec areny sprawdza, że nic z niej nie zostało w użyciu
  (ucieczka to błąd z linią); w buildzie do wydania liczenie dla aren znika (czysty bump, reset w O(1)). Sterta jako
  jedna wielka tablica jak `ARENA_BACKEND_FLAT` z holyc (rezerwacja mmap, strony przy zapisie); uchwyty to indeksy,
  więc snapshot, rollback i persistent arena to kopia bloku pamięci.
- **Scheme jako wzór (decyzja 2026-10-19).** Jedna przestrzeń nazw (Lisp-1): funkcja jest wartością typu
  `(fn (int int) int)`, nazwa lokalna przesłania wszystko. Makra higieniczne w stylu `syntax-rules` (wzorzec →
  szablon, nazwy z szablonu wiązane w miejscu definicji makra, zmienne makra nie łapią nazw z miejsca użycia),
  rozwijane przed sprawdzaniem typów. Wywołania ogonowe bez wzrostu stosu, named let, `cond`, `case`; domknięcia po
  refcouncie. Bez `call/cc` i typów dynamicznych.
- **Operatory basha:** `(|> x (f a) g)` (wartość jako pierwszy argument kroku), `(| "ls" "wc -l")`,
  `(<<< tekst "sort")`, `(> tekst plik)` i `(>> tekst plik)` (na liczbach `>` dalej porównuje), `($ "date")`, `$$`,
  `$HOME`, `&&` i `||` z komendami jak w bashu i z boolami jak `and`/`or`.
- **SoA:** `(vec struktura)` trzyma pola jako kolumny (komórka c elementu i pod `dane + c * pojemność + i`);
  `get` zbiera element, `put` i `push` go rozkładają. Odczyt jednego pola wszystkich elementów to dziś nadal pełny
  `get`; czytanie samej kolumny przez kompilator to następny krok optymalizacji.
- **Sterta FLAT (2026-10-19):** sterta i arena tekstu to po jednej rezerwacji przestrzeni adresowej (mmap z
  `MAP_NORESERVE`, jak `ARENA_BACKEND_FLAT` w holyc), domyślnie po 1 GiB; system daje strony dopiero przy zapisie.
  `GAEL_HEAP=64M` ustawia inny rozmiar, a system, który odmówi, dostaje połowę. Wcześniej obie były statycznymi
  tablicami po 32 MiB: `vec` z 5 mln intów kończył się błędem, teraz działa (`examples/bigvec.gael`). Testy pamięci
  ustawiają sobie mniejszą stertę w `NAZWA.env`.
- **Pool (2026-10-20):** `(pool T)` - stała liczba slotów (`(pool 1000)`, typ z miejsca użycia), elementy jako
  kolumny jak w `vec`, wolne sloty na liście. `(spawn p x)` daje `(handle T)`: slot i generację, jaką slot miał, gdy
  element wszedł. `get` i `put` ze starym uchwytem to błąd czasu wykonania, `lookup` daje `none`, `has` mówi, czy
  element jest, `remove` wyjmuje go (opcja) i zwalnia slot (nowa generacja), `len` liczy, `(for (h p) ...)` idzie po
  uchwytach w kolejności slotów. Uchwyt to zwykła wartość: `=`, pole struktury, klucz mapy. Kompilator pilnuje typu
  uchwytu: `(get wrogowie uchwyt-cząsteczki)` to błąd kompilacji. Generacja ma 32 bity (ten sam slot użyty 2^31 razy
  wraca do starej generacji). Na razie `get` kopiuje cały element; czytanie i zapis jednego pola w miejscu to
  następna optymalizacja, wspólna z `vec`.
- **Arena (2026-10-20):** `(arena ciało...)` - stringi, `vec`, `map` i `pool` zrobione w ciele (także w funkcjach,
  które woła) biorą komórki z regionu bump za stertą; region wraca w jednym kroku, gdy ciało się kończy. Areny się
  zagnieżdżają (stos regionów). Obiekty areny też są liczone, więc koniec areny wie, czy coś z niej jest jeszcze
  trzymane: wtedy błąd czasu wykonania z linią areny (wyciek do globalnej przez wywołanie, cykl). Kompilator zgłasza
  ucieczki, które widzi: `set`, `push`, `put`, `spawn`, `insert` wartości zrobionej w arenie do zmiennej spoza niej
  oraz wartość samej areny. `(keep x)` kopiuje to, co `x` trzyma z najbliższej areny, na stertę (obiekt trzymany
  dwa razy raz, cykl się kończy). `vec` z zewnętrznej areny, który rośnie w wewnętrznej, dostaje nowy blok na
  stercie. Pomiar (gcc -O2, 5 przebiegów, 100k klatek z `split`, mapą i `join`): ~184 ms w arenie, ~216 ms bez
  niej. Tryb wydania (arena bez liczenia, reset bez sprawdzania) jeszcze nie: najpierw pomiar, ile da ponad to.
- **Typy o rozmiarze (2026-10-20):** `i8`, `i16`, `i32`, `i64` (= `int`), `u8`, `u16`, `u32`, `u64`, `f32`, `f64`
  (= `float`), „można określać, ale nie trzeba”: bez adnotacji jest `int` i `float`. Arytmetyka zostaje w swoim
  rozmiarze i się zawija (`WRAP` po operacji na intach węższych niż 64 bity, `F32` zaokrągla), `u64` ma własne
  dzielenie i porównania. Literał przyjmuje rozmiar oczekiwany w miejscu (zadeklarowany typ, parametr, pole, drugi
  operand) i musi się w nim zmieścić - `(def b : u8 300)` to błąd kompilacji; literał powyżej 2^63 - 1 pasuje tylko do
  `u64`. Konwersje tylko jawne: `(u8 x)`, `(f32 x)`, `(int x)`...; między intami zawinięcie, float do inta poza
  zakresem to błąd czasu wykonania. `%d` i `%f` biorą każdy rozmiar; `f32` drukuje się najkrótszym zapisem, który
  wraca do tej samej wartości (`0.1`). Typy SIMD (`vec4` itd.) przyjdą osobno.
- **Funkcje jako wartości (2026-10-20):** jedna przestrzeń nazw: nazwa funkcji to wartość typu `(fn (int) int)`
  (numer funkcji + 1, zero znaczy brak), zmienna, pole albo wyrażenie takiego typu woła się jak funkcję
  (`(b.on-click 2)`, `((get handlers 0) 5)` - wyrażenie liczy się przed argumentami), a lokalna nazwa przesłania
  funkcję. `(fn ((x : int)) : int ciało...)` to funkcja bez nazwy, kompilowana w środku otaczającego kodu ze skokiem
  nad nią; wtedy nie widziała lokalnych zmiennych dookoła - domknięcia przyszły później (niżej).
- **Operatory (decyzja 2026-10-20):** operatory to tylko arytmetyka i porównania (`+ - * / % < <= > >= = !=`); reszta
  ma słowne nazwy. Logika: `and`, `or`, `not`, `nand`, `nor` (krótkie spięcie), `xor`, `xnor`. Bity intów:
  `bit-and`, `bit-or`, `bit-xor`, `bit-not`, `shl`, `shr` (licznik od 0 do liczby bitów - 1, inaczej błąd czasu
  wykonania; `shr` zachowuje znak typu ze znakiem); literały `0xff` i `0b1010`. Operatory basha zastąpione
  funkcjami: `|>` → `chain`, `|` → `pipe`, `<<<` → `pipe-in`, `&&`/`||` → `and`/`or` z `(run "cmd")`, `>`/`>>` na
  stringach → `write-file`/`append-file` (`>` znowu tylko porównuje), `($ ...)` → `sh`, `$$` → `(pid)`, `$HOME` →
  `(env "HOME")`.
- **`$` jak w GDScripcie (decyzja 2026-10-20):** `$Gracz/Sprite` to wartość typu `NodePath` (stała programu,
  porównywalna, klucz mapy, drukuje się jako `$Gracz/Sprite`). Węzeł, na który wskazuje, przyjdzie z drzewem sceny.
- **Adnotacje `@` (decyzja 2026-10-20):** stoją przed formą, jak w GDScripcie: `@export (def speed : float 10.0)`
  oznacza globalną zmienną dla inspektora edytora; nieznana adnotacja albo `@export` przed czymś innym niż `def` to
  błąd kompilacji. `gael -x PLIK` wypisuje eksporty (`nazwa : typ = wartość`, wartość gdy jest literałem) - tekstowy
  inspektor; edytor weźmie tę samą tabelę. Później: `@export` na polach struktur i nodach, adnotacje z argumentami
  (`(@export-range 0 100)`), `@onready` i `@tool` z drzewem sceny.
- **Wywołania ogonowe (2026-10-20):** wywołanie, którego wartość jest wartością funkcji i po którym nic już się nie
  dzieje (ostatnia forma ciała, a stamtąd obie gałęzie `if` i koniec `do`, `let`, `when` i ramienia `match`), to
  `TAILCALL`: kompilator zwalnia przed nim parametry, lokalne i wartości trzymane przez `match` dookoła, a VM
  przesuwa argumenty na początek bieżącej ramki i skacze. Rekurencja w tym miejscu to pętla bez limitu
  (`examples/tailcalls.gael`: milion wywołań, wzajemna rekurencja, przez wartość funkcji - `TAILCALL_V`; build sprzed
  zmiany kończy się tam przepełnieniem stosu). Typ wyniku musi być ten sam co funkcji. Nie są ogonowe: ciało `arena`
  (po nim jest koniec areny), `while`, `for`, argumenty, `and` i `or`. Czas bez zmian (2500 × 4000 wywołań plus
  `fib 27`: ~254 ms wobec ~266 ms, w granicach szumu).
- **Makra higieniczne (2026-10-20):** `(defmacro nazwa (wzorzec szablon)...)` jak `syntax-rules`: pierwsza reguła,
  której wzorzec pasuje do użycia, przepisuje je, a kompilator sprawdza typy tego, co wyszło. We wzorcu (po `_` w
  miejscu nazwy makra) symbol to zmienna wzorca, `_` pasuje do wszystkiego, keyword (`:else`, `:in`), liczba i string
  tylko do siebie, `...` po elemencie - zero lub więcej razy (jedno na listę, elementy po nim wolno). Zamiast listy
  literałów ze Scheme'a są keywordy, które gael już ma. Higiena przez stempel: każdy symbol wniesiony przez szablon
  dostaje stempel swojego rozwinięcia, lokalna jest wiązana pod nazwą i stemplem, a symbol widzi tylko lokalne ze
  swoim stemplem - `tmp` ze `swap` nie łapie `tmp` użytkownika, a wolna nazwa w szablonie (funkcja globalna, inne
  makro) znaczy to, co na górnym poziomie, nawet gdy w miejscu użycia jest lokalna o tej nazwie. Makra są tylko na
  górnym poziomie, widoczne w całym pliku (także powyżej definicji) i w plikach, które go używają; na górnym
  poziomie makro może zrobić globalne, funkcje i typy, kilka naraz przez `(do ...)` (rozwijane przed deklaracjami).
  Błędy definicji (zmienna dwa razy, `...` bez zmiennej, za mało `...` w szablonie) są w linii definicji, błędy użycia
  (żadna reguła nie pasuje - z listą wzorców, różne długości pod jednym `...`, makro rozwijające się w nieskończoność)
  w linii użycia. Analiza pożyczania operandów traktuje użycie makra jak możliwy `set`, bo `set` może być w środku
  (test pokazał zły wynik bez tego). Koszt: 2000 funkcji z `cond` na 4 gałęzie (8000 rozwinięć) kompiluje się w
  ~30 ms wobec ~18 ms z ręcznymi `if` (po indeksie nazw niżej: ~21 i ~11 ms).
- **Indeks nazw (2026-10-20):** nazwy górnego poziomu (funkcje, globalne, struktury i enumy, makra) i nazwy
  wbudowane są w jednej tablicy haszującej (`src/names.c`, FNV-1a, adresowanie otwarte), bo dzielą jedną przestrzeń
  nazw. Wcześniej każde szukanie szło po całej tabeli i kompilacja rosła z kwadratem liczby funkcji: 1000/2000/4000
  funkcji kompilowało się w ~11/24/78 ms, teraz ~7/11/17 ms (z ~2 ms startu procesu; callgrind: 689 mln instrukcji
  → 100 mln). Nieudana jednostka REPL-a, która zabiera nazwy, odbudowuje indeks z tabel.
- **Zadania, czyli korutyny i async (2026-10-20):** `(async (f x))` uruchamia `f` jako zadanie - korutynę ze
  stosem (stackful) - od razu, do pierwszego czekania, i daje `(task T)`, gdzie T to wynik `f`. Czekanie: `(sleep s)`
  (sekundy czasu zadań), `(yield)` (do następnego kroku), `(await t)` (wynik innego zadania, typ T sprawdzony w
  kompilacji). Bez kolorowania funkcji (jak w Lua, Go, GDScripcie): czekać można głęboko w wywołaniach, funkcje po
  drodze nic nie deklarują. Pętla gry woła `(tasks-step dt)` (czas zadań rośnie o dt, każde gotowe zadanie biegnie do
  następnego czekania, wynik: ile zadań zostało), skrypt albo serwer `(tasks-run)` (do końca wszystkich, w prawdziwym
  czasie, proces śpi, gdy wszystkie śpią); `await` i `sleep` w programie głównym też puszczają zadania. Wykrywane:
  wzajemne czekanie (deadlock), `await` na sobie, `yield` poza zadaniem, `tasks-step` z zadania, czekanie wewnątrz
  `arena` (arena skończyłaby się poza kolejnością). Zadanie nie używa aren kodu, który je wznowił (to, co robi, idzie
  na stertę), więc `(arena (tasks-step dt))` w pętli klatki działa. W VM: zadanie ma własny stos wartości i ramek
  (bloki sterty, podwajane przy głębokich wywołaniach), przełączenie to zapis i odczyt rejestrów, funkcja zadania
  wraca do instrukcji `TASK_END`; czekanie programu głównego to instrukcja wykonywana ponownie po każdym puszczonym
  zadaniu, więc w C nie ma pętli wokół interpretera. Koszt dla zwykłych wywołań: +0,9% instrukcji (callgrind, `fib 24`).
  Jeszcze nie: anulowanie zadania z zewnątrz (`cancel`) i porzucenie czekającego zadania - do tego trzeba map stosu
  (które sloty i wartości na stosie trzymają referencje w każdym miejscu wywołania), żeby refcount mógł zwolnić to,
  co zadanie trzyma. Na razie zadanie kończy się samo (pętla sprawdza warunek), a z `GAEL_CHECK=1` to, co trzymają
  zadania czekające do końca programu, liczy się jako niezwolnione.
- **Gemy: manifest i zależności (2026-10-20):** gem to `gems/NAZWA.gael` albo folder `gems/NAZWA/` z `main.gael`
  (reszta przez `use`), szukany obok programu, potem w folderze gaela. Plik główny zaczyna się od manifestu
  `(gem nazwa :version "0.1" :requires (...) :provides (...) :excludes (...))`. Program włącza gemy przez
  `(enable a b)`: loader czyta manifesty tylko gemów z listy (najpierw wszystkie, żeby wymaganie spełnił też gem
  wymieniony dalej), potem ładuje każdy po tym, czego wymaga. Wymaganie spełnia gem o tej nazwie albo taki, który ją
  dostarcza (`:provides`); gdy żaden nie jest włączony, loader szuka gemu o tej nazwie na dysku. Błędy z miejscem:
  brak gemu, brak dostawcy wymagania, cykl wymagań, dwa włączone gemy dostarczające to samo („wybierz jeden”, jak
  ecs-archetype i ecs-sparse dla `ecs`), wykluczenie, zły manifest, plik jednego gemu użyty przez inny. Gem używa
  tylko swoich nazw i nazw gemów, których wymaga - sprawdzane w kompilacji przy każdym użyciu funkcji, globalnej,
  typu i makra (każda nazwa zna swój gem); program widzi wszystkie włączone. W REPL-u `(enable x)` kompiluje gem do
  działającego programu, `(disable x)` go wyłącza (błąd, gdy potrzebuje go inny włączony gem) i jego nazw nie da się
  użyć aż do ponownego `enable`; kod skompilowany wcześniej działa dalej. Jeszcze nie: zakresy wersji w wymaganiach
  (`:version` jest zapisywana), `:depends` (opcjonalne), wyładowanie kodu, zamiana jednej implementacji na drugą w
  trakcie (nazwy wyłączonego gemu są dalej zajęte), gemy natywne i procesowe, prywatne nazwy gemu.
- **Domknięcia (2026-10-20):** `(fn ...)` może używać lokalnych kodu dookoła: trzyma kopię każdej, której używa,
  zrobioną w chwili tworzenia (jak lambda w GDScripcie). `set` w środku zmienia własną kopię fn, która zostaje między
  wywołaniami (licznik działa), a `vec`, `map` i `pool` są referencjami, więc ich zawartość jest wspólna; fn zrobiony w
  pętli trzyma wartości swojej tury (bez pułapki z JavaScriptu). Fn w fn dostaje wartość przez fn pośrodku. W VM:
  wartość funkcji to jedna komórka - minus (numer + 1) dla zwykłej funkcji, uchwyt obiektu domknięcia (numer funkcji i
  trzymane wartości, opisane typem-strukturą, żeby refcount wiedział, co zwolnić) albo 0; fn z `(fn ...)` dostaje swoje
  domknięcie (albo 0) w slocie 0, a lokalne z zewnątrz czyta i ustawia przez offsety (`ENV_LOAD`, `ENV_STORE`).
  Wartości funkcji są teraz liczone jak inne uchwyty (wywołanie przez wartość ją bierze, wywołanie ogonowe kopiuje ją
  przed zwolnieniem lokalnych). Szybka ścieżka dla typów z jednym uchwytem (string, vec, map, pool, task, fn) w
  `RETAIN`, `DROP` i `RELEASE`: pętla 200 tys. wywołań przez wartość +0,4% instrukcji wobec stanu sprzed domknięć,
  `fib 24` -6,5%.
- **Następne, po kolei:** mapy stosu (anulowanie zadań), traits, tryb wydania aren (po pomiarze), bufor i okno. Numery
  wersji przesuwają się względem planu wyżej.

## Notatki → stan

Każdy punkt z notatek o godot-rs i gaelu, z tym, co się z nim stało. ✓ zrobione, → w kolejce, ○ później (często gem).

**Język**
- ✓ s-wyrażenia Lispa; typy w większości statyczne, zapisywane jak w GDScripcie (`(x : int)`), inferencja w `let`/`def`.
- ✓ błędy na etapie kompilacji jako priorytet: wszystkie naraz z linią i kolumną, wyczerpujący `match`, martwe
  ramiona, kody `%` sprawdzane, literały poza zakresem, jeden błąd = jeden komunikat.
- ✓ bytecode VM, REPL, skrypty; ○ kompilacja (AOT, JIT) jako gemy.
- ✓ `printf`/`println` (`print`, `println`, `format`, `%v`), `option`, krótkie spięcie `and`/`or`, `&&`, `||`, `|>`.
- ✓ listy/array/dict/map/table: `vec` i `map`; ✓ hashmap.
- ✓ typy o określonym rozmiarze, „można określać, ale nie trzeba” (`int` = `i64`, `float` = `f64`).
- ✓ automatyczne SoA zamiast AoS na poziomie języka: `(vec struktura)` trzyma każde pole w osobnej kolumnie.
- ✓ to, co dawały operatory basha, jako funkcje (decyzja 2026-10-20: operatory to tylko arytmetyka i porównania):
  `chain`, `pipe`, `pipe-in`, `run`, `sh`, `system`, `env`, `pid`, `write-file`, `append-file`; ✓ klej jak w bashu:
  `args`, `getenv`, pliki. ✓ logika słowami (`nand`, `nor`, `xor`, `xnor`) i bity intów (`bit-and`... `shl`, `shr`).
- ✓ część edycji tekstu z Perla: `substr`, `find`, `split`, `join`, `trim`, `byte`, `char`; ○ operatory Perla (`=~`,
  wyrażenia regularne).
- ✓ refcount (stringi z czasu wykonania, `vec`, `map`, `pool`; liczenie wstawia kompilator); ✓ pool na encje z
  uchwytami z generacją; ✓ arena per klatka, poziom i żądanie z kontrolą ucieczek; → tryb wydania bez liczenia;
  ○ persistent arena pod snapshoty.
- ✓ Scheme jako wzór: jedna przestrzeń nazw, funkcje jako wartości, wywołania ogonowe, makra higieniczne
  (`defmacro` w stylu `syntax-rules`), domknięcia; ○ named let, `cond` i `case` w bibliotece standardowej (makra).
- ✓ makra; ○ makra gemu ecs (`ecs-query`), traits, abstrakcje bez boilerplate i bez pułapek C++.
- ✓ zasada nazw: typy i formy gemu z prefiksem systemu (`ECSEntity`, `(ecs-query ...)`, `DBQuery`).
- ✓ x-macro dla opkodów (`src/ops.h`).
- ✓ async/await, korutyny (zadania ze stosem: `async`, `await`, `sleep`, `yield`, `tasks-step`, `tasks-run`);
  ○ anulowanie zadań (mapy stosu), joby (fine-grained, master/slave) na wątkach - gemy.
- ○ typy deltatime, tick, tag, uuid; ○ typy SIMD i cache-friendly; ○ kluczowa matematyka z czasem do rdzenia w C.
- ✓ `@export` przed `def` jak w GDScripcie, `gael -x` jako tekstowy inspektor; ○ „lazy export” (eksport bez pisania typu), adnotacje na polach i nodach.
- ✓ moduły (`use`); ○ gemy rozszerzają typy i pętle, rdzeń daje podstawy.
- ○ inspiracje TempleOS (shell = REPL, typy holyc już są w C).
- zasady: im mniej logiki w C, tym więcej w gaelu; single-header libs; rdzeń w C może się rozwijać z własną
  wersją (wersja rdzenia osobno od wersji języka).

**Gemy**
- ✓ manifest gemu (`:requires`, `:provides`, `:excludes`), `(enable ...)` i `(disable ...)`, wybór jednej
  implementacji spośród dających to samo, wywołania między gemami sprawdzane w kompilacji; ○ wszystko jest gemem,
  wbudowane tylko kernel i editor; `:depends`; wersjonowanie; wybór gemów przy nowym projekcie (2d, 3d, sdf, aabb,
  noise); gemy dają nody; build gry wycina nieużywane gemy.
- ○ gemy w dowolnym języku (C, Rust, C++ - wejście C ABI; procesowe przez stdin/socket).
- ○ prosty menedżer pakietów: gemy na Twoim GitHubie, Ty decydujesz o merge'ach, gałąź `experimental`.
- ✓ start ładuje tylko włączone gemy (liczba zainstalowanych nie ma znaczenia); ✓ gemy włączane i wyłączane w REPL-u
  w trakcie działania; ○ to samo z kodu (tryby edytora), wyładowanie kodu.
- ○ ustawienia per projekt i globalne (gemy, pluginy, assety); profile zmieniane w trakcie działania.
- ○ katalog: soulslike, voxel, input buffer, parallax, dialogi w Markdownie, event flags, priority i event queue,
  fsm, hfsm, A*, voronoi, perlin, LOD, HLOD, raycast, raytrace, world-env, timeact editor, edytor poziomów jak w
  Quake'u, kryptografia, obfuskacja, ftl+, kalkulator, email, rss, circuitboard, druk 3D, dystrybucja gier, gry
  terminalowe.

**Edytor**
- ✓ kolejność: najpierw język, potem edytor emacslike w samym gaelu, częściami.
- ○ bufor uniwersalny: tekst, 3D, dźwięk, wideo, nody; bufor tekstu podrasowany, bufor 3D jako gem.
- ○ magit, evil, org, mail, todo, dired, shell, tryb gamedev, hooki; ○ AI/LLM od początku (Claude Code, Cursor, Ollama).
- ○ natywne GUI dla początkujących (układ Godota/GDStudio) i plugin Emacsa dla zaawansowanych.
- ○ inputy zapisane jak w Emacsie, z pressed/released jak w Godocie; pióra, tablety, pady, dotyk.
- ○ UI: Cassowary z procentami, animacje sprężyną z tłumieniem, logika UI osobno od krytycznej.

**Silnik**
- ○ wgpu, rapier, ECS na tagach, GAS z Unreala, DOTS z Unity, record / replay / rollback / snapshot.
- ○ MATS (materiały: tłuczone szkło, podpalany olej, matcap, vertex animation, Phong, Blinn-Phong, POM/SPOM).
- ○ tekstury i modele z kodu jak w kkrieger: graf jak w Blenderze / Material Makerze, kompilowany do kodu.
- ○ asset management (wersjonowanie lokalne i globalne, wzorowane na Lore); integracje z Blenderem, GIMP-em,
  Kritą, Inkscape'em, OpenSCAD-em, Audacity, REAPER-em, FL Studio, TrenchBroomem; ekosystemy Python, Mojo, Rust,
  Go, Rails.
- ○ build gry kompilowany; serwer i web jako część tego samego (fullstack).
- ✓ najpierw Linux natywnie; ○ Vulkan/OpenGL/Metal/WASM/Android/Windows przez wgpu później.
- ○ wydania: stable co określony czas z większą liczbą funkcji, testing na bieżąco.

## Ryzyka, uczciwie

- **Skala:** własny język, edytor, silnik, system gemów i zamienniki Blendera to lata pracy jednej osoby. Każdy etap
  musi być użyteczny sam w sobie, żeby coś wyszło na zewnątrz, zanim powstanie całość.
- **Ekosystem:** Godot ma ponad dekadę i tysiące addonów. Wygrać można skupieniem (Emacs, gemy, Twój workflow,
  wydania stable), nie równaniem funkcji.
- **Kod pisany przez AI:** rdzeń gaela (VM, type checker) musisz rozumieć sam - przy utrzymaniu i na rozmowach.
