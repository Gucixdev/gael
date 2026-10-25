# gael - plan

Co robię, po kolei. Decyzje i ich powody są w `docs/SPEC.md`, opis języka w `docs/REFERENCE.md`. Odhaczam tutaj, gdy
krok jest w repo i `./test` przechodzi.

Zasada nadrzędna (2026-10-25): gaela piszą głównie modele (Claude i mały model na jego sprzęcie). Każda zmiana ma
pomagać modelowi pisać kod szybki i bezpieczny: błąd mówi, co wpisać; jeden sposób na rzecz; oczywisty kod jest szybki;
dokumentacja mieści się w kontekście.

## Teraz

- [x] Wyniki `./bench` po ostatnich zmianach w `benchmarks/RESULTS.md` (`1565df1`).
- [x] `task` → `coroutine`: typ `(coroutine T)`, `step-coroutines`, `run-coroutines`, `coroutine-time`; `async`,
      `await`, `sleep`, `yield`, `done?` zostają.

## 1. Język pod modele

- [ ] Dwa poziomy, jeden format `plik:linia:kolumna: error|warning: co jest źle; jak poprawić`. Error zatrzymuje
      zawsze. Warning (nieużyta zmienna lokalna - `_` na początku nazwy ucisza; wartość wyliczona i wyrzucona, np.
      `(+ x 1)` jako instrukcja; kod po `return`) pokazuje się w `run`, REPL-u, `gael check` i edytorze, a program
      działa dalej; `gael build` (wydanie) z ostrzeżeniem we własnym kodzie się nie buduje. Bez flag. Test na każdy.
- [ ] Znaczniki `@` żyją tylko w środowisku deweloperskim (REPL, `run`, `check`, `test`, edytor), `gael build` je
      wycina: `@warn` i `@err` (komunikat przy linii i na liście, niczego nie blokuje), `@test` (blok, który uruchamia
      `gael test`), `@debug` (kod tylko w trakcie pracy). Nieznany znacznik albo w złym miejscu to błąd.
- [ ] `(compile-error "...")` dla makr: błąd kompilacji z własnym komunikatem, gdy makro dostało złe argumenty.
- [ ] Nawiasy: przy brakującym `)` wskazać linię, w której wcięcie mówi, że forma się skończyła („brakuje `)` na końcu
      linii 2”); przy nadmiarowym - która forma już się zamknęła.
- [ ] Przegląd wszystkich komunikatów: plik:linia:kolumna, co jest źle, poprawna forma.
- [ ] Ściąga dla modeli (`docs/MODELS.md`, 2-4 tys. tokenów) i skill `gael` w claude-config; test pilnuje, że każda
      nazwa z `gael -n` jest w ściądze.
- [ ] `gael -m`: rozwinięte makra, żeby było widać, co z nich wychodzi.

## 2. Jedna binarka i build system

- [ ] `gael build`: jeden plik wykonywalny - kopia gaela z doklejonym programem; działa bez gaela i bez gcc.
- [ ] Plik projektu `(project NAZWA :version ... :main ... :requires (...))`, `gael new`, `gael run`, `gael check`,
      `gael test` (pliki `.out` jak w `examples/` i bloki `(@test "nazwa" ...)` w kodzie).
- [ ] Gemy z gita i ze ścieżki: `gael get`, plik lock (commit i hash), przebudowa tylko tego, co się zmieniło.

## 3. Język

- [ ] `load` / `reload` w działającym programie: podmiana funkcji na żywo z pliku.
- [ ] Generyki (kopia na każdy typ, zero kosztu) i `result`.
- [ ] Biblioteka w gaelu na generykach: `sort`, `sort-by`, `min`, `max`, `range`.
- [ ] Traity jako zbiór haków (`Typ.nazwa`), sprawdzane w kompilacji; comptime: kod wykonywany w trakcie kompilacji
      przez ten sam VM.
- [ ] Wątki: masterslave i fgjob (fine-grained jobs) obok korutyn.

## 4. Kod natywny, w tej samej binarce

- [ ] JIT bazowy x86-64: bytecode → kod maszynowy w pamięci, sprawdzenia zostają, podmiana na żywo przez tablicę
      funkcji. Pomiar względem VM, Lua i C.
- [ ] JIT optymalizujący: SSA, przydział rejestrów, inlining, wycinanie sprawdzeń, które da się udowodnić.
- [ ] AOT: ten sam generator, kod maszynowy w pliku z `gael build` zamiast bytecode'u.
- [ ] arm64, riscv64, WASM; opcjonalnie LLVM (tekst `.ll`), gdy jest zainstalowany.

## 5. Potem

- [ ] Gem FSM/HFSM napisany w gaelu.
- [ ] artemacs: rdzeń w C (okno, wejście, rysowanie), reszta w gaelu; tryby jako gemy.
- [ ] GPU (SPIR-V).
- [ ] Ownership (owned / borrowed / shared) - opcjonalny, jeśli liczniki, areny i uchwyty nie wystarczą.

## Zrobione ostatnio

- [x] Szybszy VM: computed goto, superinstrukcje, warunki jako skoki, nowa mapa (`173d9e9`, `e7286ba`).
- [x] Bool jako bit w vecach i pulach (`7fa7bed`, `88e71bd`).
- [x] `./bench` względem C, Lua, Pythona, Lispów, JS i GDScriptu.
- [x] Metody i haki, wartości domyślne pól, x-makra (`string-of`, `name-of`), funkcje matematyczne.
