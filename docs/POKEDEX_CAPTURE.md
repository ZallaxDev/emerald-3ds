# Pokédex tras una captura: layout 2D durante combate voxel

## Diagnóstico

`DisplayCaughtMonDexPage` crea una ficha temporal dentro del combate.
`gMain.inBattle` continúa siendo verdadero: no identifica qué pantalla está
visible. En el estado 0, `Task_DisplayCaughtMonDexPage` guarda el VBlank de
combate en `gPokedexVBlankCB` y lo borra durante la preparación. En el estado
4 restaura ese mismo callback para actualizar la ficha. No instala
`VBlankCB_Pokedex` ni entra en `CB2_Pokedex`.

El puente solo reconocía la Pokédex por `VBlankCB_Pokedex`. Al recuperar el
VBlank prestado, seleccionaba el layout de batalla y volvía a componer el
escenario voxel con los fondos/texto de registro: ficha desplazada/duplicada
sobre el escenario. Registrar ese callback como Pokédex sería incorrecto:
también lo usa el combate al salir de la ficha.

La selección de `CTR_CENTRED_POKEDEX` por sí sola tampoco basta. La ficha
usa BG2 para texto/huella y BG3 para el marco (`sNewEntryInfoScreen_BgTemplate`,
mapas 32×32, prioridades 2/3). `CtrPokedex_Backdrop` devuelve -1 durante captura:
no existe `sPokedexView`/`CB2_Pokedex`. `CentredFillOf` recurría entonces al
fondo más profundo y `DrawCentredMargins` repetía sus filas/bordes, incluido
el marco, en vez de una franja limpia. Además, `VBlankCB_Battle` vuelve a
escribir BG2HOFS/BG2VOFS/BG3HOFS/BG3VOFS desde `gBattle_BG*` después del reset
de preparación. Un desplazamiento prestado hace que tanto `DrawLayerRect`
como `DrawTextSpanAt` lean el mapa con wrap de 256 píxeles: puede partir el
texto y mostrar su continuación a la izquierda. La columna cian es el área
de botones que el layout de Pokédex del campo reserva; no pertenece a la ficha.
La foto física identifica los síntomas, no los valores exactos de registros.

## Solución y límites

- [0054-pokedex-capture-layout.patch](../patches/pokeemerald/0054-pokedex-capture-layout.patch)
  añade `CtrPokedex_IsCaughtMonPage` dentro del bloque `PLATFORM_3DS` existente.
  Detecta la tarea de preparación desde el estado 1, la tarea de entrada por
  A/B y la tarea de salida. El estado 0 que espera el fade inicial conserva
  la presentación del combate, pues todavía no ha reemplazado sus fondos.
- [3ds_game_full.c](../3ds_port/src/3ds_game_full.c) consulta ese hook en
  `UpdateStage` y prioriza `CTR_CENTRED_POKEDEX_CAUGHT`: la ficha GBA se compone en
  2D en toda la pantalla inferior, sin columna de botones. El compositor existente
  puede conservar la última imagen superior; no renderiza nuevos frames del
  escenario de batalla durante la ficha.
- Durante preparación, entrada, espera de A/B y fade de salida, las peticiones
  de stage, batalla y transición quedan desactivadas, incluso con VBlank NULL.
  El render voxel de combate depende del modo batalla y no se solicita.
- Los registros de callbacks no se contaminan ni se borran. Al destruirse la
  tarea de salida se recupera automáticamente el layout del callback actual,
  incluido el VBlank de combate que seguía instalado. La lógica nativa de
  captura, sprites, paletas y `gMain.inBattle` permanece intacta.
- No hay flag persistente del port que pueda quedar activo tras un reset: el
  hook observa las tareas del juego. La Pokédex del campo conserva su ruta.
- [src/video/centred.inc](../3ds_port/src/video/centred.inc) fija un fill propio: solo BG3,
  celda (0, 2), la misma franja que PAGE_INFO. `StorageCompose` centra la imagen
  240×160 con márgenes de 40 píxeles en una salida 320×240; BG2 y sprites quedan
  recortados al rectángulo nativo. No se repiten texto, huellas ni bordes.
- `Reg` devuelve scroll cero para BG2/BG3 únicamente en ese modo. Las rutas
  de textura y tiles comparten la lectura, incluso con registros por scanline.
  `LineBackgrounds` impide que DMA de scroll heredado seleccione `DrawLineBg`
  y salte el fill/recorte de la ficha; tampoco borra las tablas de batalla.
  No se escriben registros del juego ni variables `gBattle_BG*`: al cerrar o
  volver al campo se recuperan sus valores originales sin restauraciones manuales.
- [3ds_video.h](../3ds_port/include/3ds_video.h) añade el modo al final del enum,
  sin cambiar los IDs existentes. `BottomWhole` elimina la columna cian mediante
  la ruta existente de pantalla completa, sin tocar la UI inferior.
- No se modifica `0005-port-hooks.patch` ni la lógica nativa de captura.
  Se preserva la modificación concurrente de moonlight en el renderer.
  Las builds finales EN/ES integran el hook, el puente y el renderer coherentes.

## Integración manual en árboles staged

La integración posterior necesita 0043 después de 0042 y las versiones
coherentes de `3ds_game_full.c`, `3ds_video.c` con `src/video/` y `3ds_video.h`. Copiar solamente
el puente antiguo deja el renderer equivocado; copiar el puente nuevo sin
el header/renderer no integra el nuevo modo. Antes de integrar en otro árbol,
conservar los cambios concurrentes de cada
árbol y comprobar si 0043 ya está aplicado mediante `git apply --reverse --check`.

No hace falta copiar `port_platform.h`. No ejecutar bootstrap ni reset/clean
de los árboles staged durante la integración concurrente con moonlight. El
bootstrap de un árbol nuevo ya descubre 0043 por orden numérico.
Recompilar los objetos de `src/pokedex.c`, `3ds_game_full.c` y `3ds_video.c` con el flujo
Docker habitual (`emerald3ds-build:local`). Las builds finales EN/ES pasan
`make verify` y la auditoría ARM11/enlazado/recursos.

## Regresión específica

[pokedex_capture_test.py](../3ds_port/tests/pokedex_capture_test.py) extrae y
compila las funciones reales del puente y las tareas nativas de captura en un
harness C sin SDK/PICA. Si falta 0043 en `--tree`, comprueba y aplica el patch
**solo a una copia temporal**, sin escribir en ese árbol. Por defecto prueba
el puente del repositorio; `--bridge` permite verificar la copia staged.
`--video` selecciona el renderer; por defecto se usa el del repositorio.

```sh
python3 3ds_port/tests/pokedex_capture_test.py --tree build/upstream
python3 3ds_port/tests/pokedex_capture_test.py --tree build/upstream-es
python3 3ds_port/tests/pokedex_capture_test.py --tree build/upstream --sanitize
python3 3ds_port/tests/pokedex_capture_test.py --tree build/upstream-es --sanitize
# Después de integrar, verificar también el puente copiado:
python3 3ds_port/tests/pokedex_capture_test.py --tree build/upstream \
    --bridge build/upstream/3ds_port/src/3ds_game_full.c
```

El test verifica espera de fade antes de abrir, todos los estados de
preparación, VBlank NULL, recuperación del VBlank prestado, espera de entrada,
fade de salida, A y B, capturas repetidas, dos callbacks distintos de batalla,
retorno al combate con el mismo callback y Pokédex normal/campo. Comprueba que
`gMain.inBattle` sigue verdadero y que el callback de combate nunca se añade
al conjunto centrado. ASan/UBSan abortan ante un error.

Un segundo harness compila las funciones reales `Reg`, `CentredFillOf`,
`DrawCentredMargins`, `DrawTextSpanAt`, `DrawLayerRect`, `DrawTextBg`,
`StorageComposePart` y `StorageCompose`, con rasterización software de las
llamadas de dibujo. Compara los 76.800 píxeles de salida usando tiles sintéticos
distinguibles: BG2 solo en 240×160, BG3 solo con su franja en los márgenes,
un sprite dentro y otro oculto fuera. Exige paridad entre textura y fallback de
tiles, con scroll 0/7/80/240/256/511 y registros scanline heredados. Comprueba
que ni los registros de entrada ni la Pokédex normal/combate se alteran.
No es una prueba del driver PICA ni una captura real de framebuffer.

En consola/Azahar comprobar una especie nueva con voxel y combate voxel
activados: registro legible, sin wrap, duplicados ni columna cian, sin nuevas capas
de escenario detrás de la ficha; cerrar con A y B y comprobar la continuación
nativa de captura/apodo. Repetir otra captura nueva, una especie ya registrada,
voxel desactivado y Pokédex del campo. El usuario confirma que la versión
final funciona en su 3DS y autoriza publicarla el 12 de octubre de 2026.
No se proporcionaron mediciones de FPS ni un desglose por escenario; se
registra aceptación general, no resultados individuales inventados.

Durante la primera prueba UBSan se observó un desplazamiento con signo previo
en la reconstrucción nativa de `personality` al usar una mitad alta mayor que
`0x7FFF` en `Task_ExitCaughtMonPage`. No se modifica aquí, por quedar fuera del
bug de layout; los IDs del harness evitan ese caso para aislar esta regresión.
