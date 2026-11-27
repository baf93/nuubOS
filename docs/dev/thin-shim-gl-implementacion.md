# Implementación de `libgl_nuubos_shim.so` (especificación v2.1) — seguimiento

Estado: **FINAL / IMPLEMENTADO Y DESPLEGADO EN EL DISPOSITIVO DEV**. Cierre funcional: 2026-11-26. Incluye la implementación inferida del Módulo H (completada con listas de visualización y alias ARB/EXT), el runtime armhf con **Mesa/DRI de 32 bit** y los backports Mesa `0012`/`0013` **aplicados y desplegados por decisión del usuario**. Decisiones cerradas por el usuario, documentadas en §"Decisiones cerradas" y **no reabrir salvo petición explícita**: G.3 queda como **subconjunto opt-in**, el coste de **Mesa/DRI de 32 bit queda aceptado** y `0012`/`0013` se conservan **sin reclamar mejora medible**. La auditoría de cobertura de la spec v2.1 está cerrada: no queda ninguna función real ni alias exigido sin cubrir; los únicos nombres residuales son familias genéricas (`glColor*`, `glGet*`, …) o elementos que la propia spec declara fuera de alcance.

## Contexto y reglas de trabajo

- Entrada: `docs/dev/especificacion_thin_shim_gl_nuubos_v2_1.txt` (2552 líneas, v2.1), tratada como **entrada inmutable**: no se ha reescrito ni truncado.
- El propio documento indica en su `ANEXO v2.1` que **termina en el Módulo G**: no incluye el Módulo H, las secciones 4–6 ni los Apéndices A–E, aunque los referencia. El trabajo se guió por el texto disponible y por ese anexo; las lagunas se recogen al final.
- El documento antiguo `/home/christian/Documentos/Untitled 1.txt` (v2) también **termina en el Módulo G**: añade detalle de la v2 (EOF/retícula de Módulos A–G, hooks EGL, estados legacy), pero **no contiene** el Módulo H, las secciones 4–6 ni los Apéndices A–E. No era, por tanto, la fuente de las partes ausentes. Se cogió de él solo lo ya reflejado en la v2.1/anexo: exportaciones con nombre real, alias `libGL.so.1` como decisión de distribución, y la política de extensiones/capacidades reales.
- El `ANEXO v2.1` aporta cambios concretos que sí se han aplicado: variables `NUUBOS_SHIM_FF_SHADERS`/`GEOM_EMU`/`TESS_EMU`/`GEOM_CAP_MB`/`TESS_MAX`; wrappers `glLight*`, `glMaterial*`, `glFog*`, `glClipPlane`, `glColorMaterial`, `glUniform*d(v)`, `glDrawRangeElements`, `glLinkProgram`, `glUseProgram`; constantes G.3; y la Fase 0 con umbrales de atributos/UBO/compute/TF.
- El usuario pidió ejecutar todo sin parar a preguntar; las dudas se recogen al final.
- Requisito añadido durante la ejecución: **soporte para juegos 32 bit de PortMaster**. Se comprobó primero la toolchain disponible: en `nuubos-dev:0.5` no existía soporte armhf, así que se añadió `gcc-arm-linux-gnueabihf` al contenedor. No se duplicó una toolchain ya presente.
- Restricciones del repo (CLAUDE.md + `docs/dev/principles.md`), todas respetadas:
  - Compilación **solo** con `./scripts/compile.sh` en Docker, dirigida (`gl-nuubos-shim-rebuild`, `nuubos-ports-rebuild`, `armhf-runtime-rebuild`).
  - **Nunca** se borró `output/` ni se usó `--clean`; no se compiló desde cero.
  - Se comprobó que no hubiera otro build en marcha antes de cada compilación.
  - Despliegue en `root@192.168.1.135` (nuubOS `1.0.0-rc.5`, `aarch64`) de la biblioteca aarch64, la armhf y el runtime 32 bit.
  - **Commit/push:** no se hicieron hasta que el usuario lo autorizó el 2026-11-27; ver §"Cierre de la investigación del glitch".
  - No hay cadenas visibles nuevas: la interacción del shim es por entorno/trazas `stderr` bajo `NUUBOS_SHIM_DEBUG`; no se tocó i18n.

## Plan (hitos)

1. [x] Leer `principles.md`, índice `docs/dev/README.md`, la especificación disponible y el documento antiguo.
2. [x] Inventariar estado del repo: paquetes, `compile.sh`, `nuubos-ports`/`bash-env.sh`, `labwc`, reglas udev, shim Mono anterior.
3. [x] Diseñar estructura de fuentes y build multiarch dentro de los patrones existentes.
4. [x] Implementar hooks core, infraestructura (3.0) y módulos A–G disponibles, con G.2 y emulación opt-in G.3 según el texto.
5. [x] Integración con el SO: `BASH_ENV`, Wayland/S3TC y `ld.so.conf` armhf.
6. [x] Pruebas de fase 0 y de aceptación automatizables.
7. [x] Punto 3: runtime armhf real en el dispositivo y calificación con Apotris (PortMaster 32 bit).
8. [x] Punto 2: decisión de compatibilidad de G.3 frente a otras medidas (runtime, `libGL.so.1`, gl4es).
9. [x] Compilación dirigida en Docker, despliegue y verificación en el dispositivo.
10. [x] Documentar resultados, enlazar desde `docs/dev/README.md` y cerrar; commit/push autorizados por el usuario el 2026-11-27.

## Qué se implementó

### Paquete y build

- Nuevo paquete `package/nuubos/gl-nuubos-shim/`:
  - `src/nuubos_gl_shim.c`: fuente canónica del shim.
  - `gl-nuubos-shim.mk`: compila dos variantes; instala `/usr/lib/libgl_nuubos_shim.so` (aarch64), `/usr/lib32/libgl_nuubos_shim.so` y `/usr/lib/arm-linux-gnueabihf/libgl_nuubos_shim.so` (ambos armhf), más `GL_NUUBOS_SHIM_BIN_ARCH_EXCLUDE = /usr/lib32/` y `/usr/lib/arm-linux-gnueabihf/`.
  - `libGL.so.1` en los tres directorios, enlace a `libgl_nuubos_shim.so`: da soname a las apps con `DT_NEEDED libGL.so.1` que no traen gl4es. No proporciona GLX (no hay X11 en nuubOS).
  - `armhf-ld.so.conf` instala `/etc/ld.so.conf.d/40-nuubos-armhf.conf` con `/usr/lib32`, para launchers antiguos que buscan ese path; el loader armhf encuentra el shim por defecto en `/usr/lib/arm-linux-gnueabihf`, sin caché de 32 bit.
  - `LICENSE` GPL-3.0-or-later.
- `Config.in` y `configs/nuubos_rg35xx_pro_defconfig`: opciones `BR2_PACKAGE_GL_NUUBOS_SHIM=y` y `BR2_PACKAGE_ARMHF_RUNTIME=y`.
- `package/nuubos/nuubos-ports/Config.in`: `select` de ambos paquetes (la capa PortMaster necesita el shim y el runtime 32 bit).
- `docker/development/Dockerfile`: toolchain `gcc-arm-linux-gnueabihf`; imagen `nuubos-dev:0.5` reconstruida.
- `bash-env.sh`: `LD_PRELOAD=libgl_nuubos_shim.so` (SONAME desnudo), guard anti-duplicados y comentario actualizado con la ruta multiarch. Mantiene Wayland, S3TC y variables readonly.
- `nm -D ... | grep -c ' T gl'`: **176** exportaciones directas, además de los hooks `dlsym`/EGL.

### Runtime armhf (punto 3)

- Nuevo paquete `package/nuubos/armhf-runtime/`:
  - `armhf-runtime.tar.xz`: userland 32 bit **precompilado**, de 6,4 MB comprimidos / ~15 MB instalados, con 140 entradas.
  - `armhf-runtime.mk`: extrae el tarball en el build dir, copia `usr/lib/arm-linux-gnueabihf` al target y crea `/lib/ld-linux-armhf.so.3 -> /usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3`.
  - `NOTICE.md`: procedencia y licencias.
- Procedencia: paquetes **Ubuntu noble armhf** descargados desde `ports.ubuntu.com` dentro de `nuubos-dev:0.5` (glibc 2.39, libgcc/libstdc++, SDL2 2.30.0, zlib 1.3 y la clausura `DT_NEEDED` de SDL2 + `libpulsecommon`). No es un build desde cero ni se han mezclado binarios de otra glibc.
- Motivo: los ports armhf de PortMaster llevan `PT_INTERP=/lib/ld-linux-armhf.so.3`; sin loader no arrancan. PortMaster ya detecta `/usr/lib/arm-linux-gnueabihf` como `DEVICE_HAS_ARMHF=Y` y antepone sus `libs.armhf` para juegos 32 bit.
- **Mesa/DRI de 32 bit incluido por decisión del usuario** (supersede la elección anterior de SDL2 software): clausura `DT_NEEDED` de Mesa 25.2.8 de Ubuntu noble armhf — `libEGL.so.1`, `libGLESv2.so.2`, `libEGL_mesa.so.0`, `libgallium-25.2.8-...so` (26 MB), `libLLVM.so.20.1` (124 MB), `libGLdispatch.so.0`, X11/glvnd, DRI `panfrost_dri.so`/`swrast_dri.so`/`kms_swrast_dri.so` — más `/usr/share/glvnd/egl_vendor.d/50_mesa.json`. Coste: tarball de 6,4 MB → 48 MB (~253 MB instalados). El shim armhf sigue siendo el `libGL.so.1` de 32 bit.
- Validación real en el dispositivo: `/tmp/mesa32-test` (ELF armhf, contexto EGL surfaceless + GLES3 + FBO) devuelve `EGL=1.5`, `GL_VERSION=OpenGL ES 3.1 Mesa 25.2.8-0ubuntu0.24.04.5`, `GL_RENDERER=Mali-G31 (Panfrost)`, `FBO_STATUS=0x8cd5`, `PIXEL=51,102,153,255 err=0x0`. Con `LD_PRELOAD=libgl_nuubos_shim.so`: `desktop=0` conserva la identidad ES; `desktop=1` anuncia `3.1 Mesa Mesa 25.2.8 ... (nuubOS Panfrost)`. No hay regresión en aarch64 (Mesa 26.2.7).
- `NOTICE.md` y `Config.in` del paquete documentan la procedencia/licencias y la nueva descripción; el tarball se regeneró desde `/tmp/opencode/armhf/root` + la clausura Mesa y se instaló con `armhf-runtime-rebuild` dirigido.
- No se necesita `ldconfig` ni `ld.so.cache` de 32 bit: cada loader busca sus rutas multiarch por defecto.

### Módulos y decisión de G.3 (punto 2)

- **A–C / E**: hooks de `dlsym`/EGL, registro de contextos, detección de características y parcheo de versiones/cadenas/extensiones. `NUUBOS_SHIM_DESKTOP_GL=0` desactiva solo los wrappers de identidad desktop; `=1` los fuerza; sin variable es automático. **La redirección de `dlopen` de gl4es no depende de esta variable**: sigue activa con `=0` (ver el punto 3).
- **D**: `glMapBuffer` → `glMapBufferRange`; `glGetTexImage` por FBO+`glReadPixels`; S3TC/BGRA.
- **F/H parcial + G.2 / G.2.5**: matriz/pila, color, normal, texcoord, texturas, clip y niebla; UBO `ShimFF` con `_Static_assert`; GLSL 1.20 → ES 300/310; modo inmediato y FF sobre programs reales.
- **G.3 (experimental, opt-in)**: `NUUBOS_SHIM_GEOM_EMU` / `NUUBOS_SHIM_TESS_EMU`. Subconjunto cerrado: GS `points|lines|triangles` → `points|line_strip|triangle_strip` con `max_vertices` literal; tessellación de hito 1 con niveles uniformes. `NUUBOS_SHIM_GEOM_CAP_MB=32`, `NUUBOS_SHIM_TESS_MAX=16`. Casos no soportados se trazan y no se dibujan. TF captura `gl_Position` con el varying sintético `shim_tf_pos` (GLES prohíbe capturarlo directamente).
- **Decisión**: **no ampliar G.3**. La revisión de PortMaster-New muestra que los títulos con GL usan gl4es (o animaciones Ren'Py redirigidas a GLES por `portsd.c`) y fixed-function/GL 1.x–2.1; el uso de `GL_EXT_geometry_shader` en ports reales no aparece. Ampliar la emulación de GS/tess no aumentaría la compatibilidad observable y sí el riesgo. En su lugar se priorizaron tres medidas con impacto real:
  1. runtime armhf (arranque de ports de 32 bit),
  2. `libGL.so.1` como alias del shim (apps enlazadas a ese soname sin gl4es propio),
  3. **redirección de gl4es**: el `dlopen` de `gl4es.aarch64/libGL.so.1` se reescribe a `libGLESv2.so.2`. Se retiró el antiguo opt-out: gl4es sobre escritorio GL dibujaba una ventana negra en nuubOS (ver `docs/dev/emulation.md` y `open-items.md`), y el shim existe precisamente para evitarlo. `NUUBOS_SHIM_DESKTOP_GL=0` ya no desactiva esta redirección.
- Regresión comprobada: con `LIBGL_FB=4` el shim imprime `desktop=0` pero el `dlopen` de gl4es sigue redirigiéndose; con `NUUBOS_SHIM_DESKTOP_GL=1` imprime `desktop=1` y pasa las pruebas de aceptación.

### Módulo H (implementación inferida)

El texto del Módulo H no está en la spec v2.1 disponible; se implementó por inferencia a partir de las referencias del `ANEXO v2.1` y de los alias citados en las secciones 4–6/Apéndices A–E. Cobertura priorizada:

- **H.1** `glPushAttrib`/`glPopAttrib`: instantánea de 16 niveles de matrices, color/normal/texcoord, estados de textura, alpha, niebla, material, clip y arrays; las máscaras sin representación en GLES (hint/line/polygon) se aceptan como no-op.
- **H.2** `glAlphaFunc` (8 comparaciones), `glShadeModel` y `glPointSize` con flag de "explícitamente fijado"; `shim_alpha_apply()` en el fragment shader traducido y en el shader FF interno.
- **H.2/H.3** matriz de textura por unidad volcada al UBO `ShimFF` y aplicada **al entrar en `main()`** (al final llegaba tarde si el shader copiaba `gl_TexCoord` a su propio varying). `glActiveTexture` pasa a envolverse y selecciona la unidad para `GL_TEXTURE_2D` y la pila de textura.
- **H.3** `glTexEnvf/fv/i/iv` con MODULATE/REPLACE/DECAL/ADD/BLEND y color de entorno; `FFD_TEXENV` en el shader FF interno.
- **Apéndice C** alias sin sufijo: `glMultiTexCoord2f`, `glFogCoordf/fv`, `glSecondaryColorPointer`, `glFogCoordPointer`, además de las variantes `ARB`/`EXT` ya presentes.
- **H.4 listas de visualización**: `glGenLists`/`glDeleteLists`/`glIsList`/`glListBase`/`glNewList`/`glEndList`/`glCallList`/`glCallLists` exportados con nombre real. Grabación compacta de comandos por contexto (`begin/end`, vértices, color, normal, texcoord, matrices, enable/disable, textura, alpha, texenv y llamadas anidadas); `GL_COMPILE_AND_EXECUTE` replaya al cerrar la lista; profundidad de anidamiento limitada a 64.
- **Apéndice D / spec 969–986**: alias exigidos por las extensiones anunciadas (`glBindFramebufferEXT`, `glGenFramebuffersEXT`, `glBlitFramebufferEXT`, `glRenderbufferStorageMultisampleEXT`, `glDrawBuffersARB`, `glVertexAttribDivisorARB`, `glDrawArraysInstancedARB`, `glDrawElementsInstancedARB`, etc.) y fallback `glDrawElementsBaseVertex{EXT,OES}`. Todos resueltos en `shim_lookup_gl`.
- **Lista mínima garantizada (spec 169–182)**: se añadieron los punteros reales que faltaban — `glClear`, `glFlush`, `glFinish`, `glGenTextures`/`glDeleteTextures`, `glBlitFramebuffer`, renderbuffers y framebuffers completos, samplers, query objects, instanced draws, `glDrawElementsBaseVertex` y `glMultiDrawArrays` — para que `eglGetProcAddress`/`dlsym` devuelvan el real sin wrapper.
- `sizeof(ShimFFBlock)==3808` se mantiene con `_Static_assert`.

### Correcciones relevantes durante la puesta a punto

- Matrices identidad y contexto inicial correctos en `eglCreateContext`.
- Binding del UBO FF y sincronización `ff_dirty`; `ff_feed_user_program` solo con FF legacy activo.
- `EmitVertex`/`EndPrimitive` reescritos como identificador, sin `()` duplicados.
- Parser de líneas G.3 con copia terminada en NUL: `strchr(q,'{')` ya no ve llaves de líneas posteriores.
- Seguimiento de VS/FS por programa para adoptar shaders adjuntados antes del GS virtual.
- `shim_vin` en el compute, `slot=(p*MAXV+shim_n)*OSTRIDE`, `out_stride`/`attr_off` reales y `EnableVertexAttribArray` para varyings.
- El build armhf/runtime necesitó `ARMHF_RUNTIME_BUILD_CMDS` para extraer el tarball: con `SITE_METHOD=local`, Buildroot rsynca el directorio del paquete y no usa la ruta de descarga genérica.

## Verificación en `root@192.168.1.135`

Salvo indicación contraria: `EGL_PLATFORM=surfaceless`, `LD_PRELOAD=libgl_nuubos_shim.so` y `NUUBOS_SHIM_DESKTOP_GL=1`.

### Fase 0

- `OpenGL ES 3.1 Mesa 26.2.7`, `Mali-G31 MC1 (Panfrost)`.
- `MAX_VERTEX_ATTRIBS=16`, `MAX_UNIFORM_BUFFER_BINDINGS=105`, `MAX_UNIFORM_BLOCK_SIZE=65536`, `MAX_COMPUTE_WORK_GROUP_INVOCATIONS=256`, `MAX_SHADER_STORAGE_BLOCK_SIZE=134217728`.
- `COMPUTE_LINK_OK=1`, `COMPUTE_DISPATCH_ERR=0x0`, `TRANSFORM_FEEDBACK_LINK_OK=1`, `PROGRAM_UNIFORM_ERR=0x0`.
- Sin `GL_EXT_clip_cull_distance`, `GL_EXT_geometry_shader` ni `GL_OES_geometry_shader`; `ES_VERSION_OK=0` y `PASS=0` porque el arnés exige ES 3.2 y extensiones ausentes. Es una limitación del arnés/hardware, no del shim.

### Aceptación

| Prueba | Resultado |
|---|---|
| `tg` T-G2-1 (FF textura+color) | `255,0,0,255` |
| `tg` GS compile / link (`GEOM_EMU=1`) | `GS_COMPILE_OK`, `LINK_OK` |
| `tg` T-G3-1 (points → triangle_strip) | `135,135,0,255` |
| `tg2` FF | `FF_PIXEL=255,0,0,255` |
| `tg3` FF + UBO | `PIXEL=255,0,0,255` |
| Regresión `GEOM_EMU=0`/`TESS_EMU=0` | `T-G2-1=255,0,0,255`; `GS_COMPILE_FAIL` esperado |
| Arnés H (`/tmp/th`) | `ALPHA_DISCARD=0,0,0,255`; `ALWAYS_RED=255,0,0,51`; `POP_DISCARD=0,0,0,255`; `TEXMATRIX=0,255,0,51`; `FF_REPLACE=0,255,0,255`; `FF_ADD=0,255,255,255` |
| `dlopen` explícito de `gl4es/libGL.so.1` | `dlopen: .../gl4es.aarch64/libGL.so.1 -> libGLESv2.so.2`; `GL_VERSION=3.1 Mesa Mesa 26.2.7 (nuubOS Panfrost)` |
| `NUUBOS_SHIM_DESKTOP_GL=0` con gl4es | `shim_init ... desktop=0`, pero la redirección de `dlopen` sigue activa |
| Arnés H.4 / alias (`/tmp/dl2`, `/tmp/dl-test`) | `DL_COMPILE_EXEC=DL_CALL=DL_NESTED=DL_CALL_LISTS=255,0,0,255`; `DL_AFTER_DELETE=0,0`; los 11 alias consultados devuelven puntero no nulo |

### Auditoría de cobertura v2.1

Se extrajeron 259 tokens `gl*` de la spec y se compararon con las 329 entradas reales del shim (exportaciones `nm -D`, `GLFUNCS`, `WRAP` y `REAL`) mediante `/tmp/opencode/audit-coverage.py`. Resultado: **27 residuales, todos no funcionales o fuera de alcance**:

- Familias genéricas de la prosa (`glColor*`, `glDelete*`, `glFramebuffer*`, `glGet*`, `glLoadMatrix*`, `glMultMatrix*`, `glMultiTexCoord*`, `glNormal*`, `glProgramUniform*`, `glRenderbuffer*`, `glRotate*`, `glSampler*`, `glScale*`, `glStencil*`, `glTexCoord*`, `glTexEnv*`, `glTexGen*`, `glTexParameter*`, `glTranslate*`, `glUniform*`, `glUniformMatrix*`, `glVertex*`): cada miembro concreto está cubierto por su variante real.
- `glPatchVertices`: nombre de la prosa derivado de `gl_PatchVerticesIn` (builtin GLSL), no una función GL.
- `glBindFragDataLocation`: declarado explícitamente **no implementado** en la spec (línea 918).
- `glPointParameter` / `gl_Point.*` y `glTexGen` / `gl_EyePlane*`, `glVertexAttribLPointer` / entradas `dvec`: listas cerradas de lo que G.2 **no cubre** (G.2.11, líneas 2073–2086).

No queda ninguna función real GLES, exportación solo-escritorio ni alias ARB/EXT de la spec sin cubrir. La regla "toda extensión anunciada DEBE tener sus funciones en `shim_lookup_gl`"(línea 970) se verifica con el arnés de alias.

### Calificación armhf

| Comprobación | Resultado |
|---|---|
| `/lib/ld-linux-armhf.so.3 --version` | `ld.so (Ubuntu GLIBC 2.39-0ubuntu8.12) stable release version 2.39.` |
| ELF 32 bit mínimo con `dlopen("libSDL2-2.0.so.0")` | `ARMHF_OK bits=32 sdl=yes` |
| El mismo ELF con `LD_PRELOAD=libgl_nuubos_shim.so` | `ARMHF_OK bits=32 sdl=yes`, `rc=0` |
| `LD_PRELOAD=libGL.so.1` en ELF 32 bit y en `/bin/true` | `rc=0` en ambos (resolución por ABI) |
| Apotris 32 bit (`Apotris.armhf`, PortMaster real) | Arranca; `Tilengine v2.15.1 32-bit release`, `shim_init: pid=... desktop=0 ...`; con `SDL_VIDEODRIVER=dummy` falla la creación de texturas, esperado sin renderer; en Wayland llega hasta el bucle del juego |
| Abuse (aarch64 + gl4es, PortMaster real) | Arranca con el shim: `Video : 640x480 24bpp (renderer: software)`, sin ventana negra; `renderer: software` es del propio juego |

### Artefactos desplegados

- `/usr/lib/libgl_nuubos_shim.so`: ELF 64-bit ARM aarch64, md5 `73b4565100e3f6212225dbfd8833a3d8` (build de diagnóstico: mismos comentarios/trazas en inglés y fix DXT `data==NULL`, más trazas acotadas de `glTexParameteri`, `glGenerateMipmap`, viewport/scissor y FBO bajo `NUUBOS_SHIM_DEBUG`); `/usr/lib/libGL.so.1` → shim.
- `/usr/lib32/libgl_nuubos_shim.so` y `/usr/lib/arm-linux-gnueabihf/libgl_nuubos_shim.so`: ELF 32-bit ARM EABI5, md5 `6ad1e653c0beef986308a6104a20d14c`; `libGL.so.1` → shim en ambos.
- **Desplegado el 2026-11-27** (fix de quads, trazas de renderer/DXT y guard de mipmaps): aarch64 `196e350f7131162fca3216152d0792eb`, armhf `71a10a93fe28b99026fb64537680e134` (ambas rutas armhf). Los md5 de arriba son los anteriores al cambio.
- **Desplegado después** (traza de `glTexSubImage2D` y modo BGRA en `shim_init`): aarch64 `0c6d269711e7ec721e00476d28989133`, armhf `afcf0f34a07d38a760913bbf94ea1235`. Los md5 `196e...`/`71a1...` son del build inmediatamente anterior.
- **Build final (2026-11-27, desplegado)**: aarch64 `804f3b04a475843f8995a396ce1a10c9`, armhf `68a16d3c5d3c8eedc92892419b1c2c0e`. Incluye las correcciones de la última pasada (§"Cierre de la investigación del glitch") y **no** incluye instrumentación de diagnóstico. Los md5 intermedios de la ronda de trazas son `073fa94d…`, `0c6d2697…`, `08c2df92…`, `0774eec6…`, `702fc97f…`, `c41fc6b8…`, `20aa607e…`, `bf7dcb20…`, `64addd53…`, `756be824…` y `daacb951…` (aarch64).
- `/lib/ld-linux-armhf.so.3` → `/usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3`, con 146 entradas armhf en ese directorio (incluye Mesa/DRI 32 bit).
- `/usr/lib/arm-linux-gnueabihf/libgallium-25.2.8-0ubuntu0.24.04.5.so`, `libLLVM.so.20.1`, `dri/panfrost_dri.so`, `dri/swrast_dri.so` y `/usr/share/glvnd/egl_vendor.d/50_mesa.json` desplegados.
- `/etc/ld.so.conf.d/40-nuubos-armhf.conf`: `/usr/lib32`.
- `/usr/lib/nuubos/ports/bash-env.sh`: preload con SONAME desnudo y comentario actualizado.
- `LD_PRELOAD=libgl_nuubos_shim.so /bin/true` → `PRELOAD_OK`.

Se limpiaron todas las trazas temporales de depuración; quedan solo los diagnósticos estables bajo `NUUBOS_SHIM_DEBUG`. Comentarios y mensajes de depuración están íntegramente en inglés (el código no cambió respecto al build funcional: solo se ampliaron comentarios y se tradujeron cadenas de diagnóstico). La compilación final no produce warnings (`-Wall -Wextra`).

## Límites conocidos (aceptados, no edge cases abiertos)

1. **Texto canónico ausente**: ni la v2.1 ni el documento antiguo traen el Módulo H completo, las secciones 4–6 ni los Apéndices A–E; solo el `ANEXO v2.1`. Toda referencia consultable de esas partes está implementada y verificada. Si aparece el texto completo, se revisaría como cambio nuevo, no como trabajo pendiente de esta auditoría.
2. **G.3 es un subconjunto por decisión**: no emula semántica GS completa (múltiples primitivas, `max_vertices` dinámico, entradas array/matriz, `gl_Layer`) ni tessellación con niveles fraccionarios (hito 2). Los casos no soportados se trazan y no se dibuja la primitiva; es el comportamiento documentado de la emulación opt-in.
3. **Apotris con `SDL_VIDEODRIVER=dummy`**: el binario y el shim cargan, pero no hay renderer y fallan las texturas. Es una limitación del arnés, no del runtime; la prueba útil es el arranque del ELF 32 bit y el `shim_init` en Wayland.
4. **Backports Mesa 26.2.7 conservados por decisión del usuario**: `0012`+`0013` siguen **aplicados y desplegados** (`libgallium-26.2.7.so` md5 `5f2dcbf458e75bc78a674ee4c3b19d3d`); el A/B no mostró ganancia reproducible y **no se presentan como mejora de rendimiento**. El baseline sigue respaldado en `/userdata/nuubos-deploy-backup-20261125/` (md5 `483263e26ce64deb5ae0afb4d0902045`). Ver `docs/dev/mesa-263-perf-candidates.md` y `docs/dev/perf-power.md`. No se ha hecho build desde cero (prohibido sin permiso explícito).
5. **Commit/push**: inicialmente se dejaron sin commitear por petición del usuario; el 2026-11-27 autorizó expresamente commitear y pushear el cierre.

## Decisiones cerradas (no reabrir salvo petición explícita)

Estas tres decisiones están **cerradas por decisión del usuario** y no deben reinterpretarse como trabajo pendiente ni como optimizaciones a aplicar en una pasada futura:

1. **G.3 es y sigue siendo un subconjunto opt-in.** Las variables `NUUBOS_SHIM_GEOM_EMU` y `NUUBOS_SHIM_TESS_EMU` (ambas a `1`) son las únicas que activan la emulación; por defecto está desactivada. El alcance es el conjunto cerrado descrito en §"Módulos y decisión de G.3": GS `points|lines|triangles` → `points|line_strip|triangle_strip` con `max_vertices` literal, y tessellación de hito 1 con niveles uniformes. No hay planes de ampliarlo a GL_EXT_geometry_shader completo ni a niveles fraccionarios: los ports revisados usan gl4es o fixed-function GL 1.x/2.1 y no hay títulos que dependan de GS/tess real. Los casos fuera del subconjunto se trazan y su dibujo se omite (comportamiento documentado, no un defecto abierto).
2. **El coste de Mesa/DRI de 32 bit está aceptado.** Se incluye en `armhf-runtime` por decisión del usuario y **supersede** la alternativa de SDL2 software analizada antes. Coste asumido: el tarball pasa de 6,4 MB a 48 MB y la instalación a ~253 MB, con `libLLVM.so.20.1` incluida. No se va a sustituir por una pila de software ni a recortar DRI/LLVM para recuperar espacio: es la base de compatibilidad de los ports armhf de PortMaster. Validación y procedencia en §"Runtime armhf (punto 3)" y en `package/nuubos/armhf-runtime/NOTICE.md`.
3. **Backports Mesa `0012`/`0013` conservados, sin reclamo de rendimiento.** Siguen aplicados y desplegados (`libgallium-26.2.7.so` md5 `5f2dcbf458e75bc78a674ee4c3b19d3d`). El A/B no mostró ganancia reproducible, así que **no se presentan como mejora medible**; se conservan por decisión del usuario, no por una conclusión de benchmark. Baseline respaldado en `/userdata/nuubos-deploy-backup-20261125/` (md5 `483263e26ce64deb5ae0afb4d0902045`).

## Regresión de entorno 2026-11-26 (cerrada)

Tras el despliegue final el usuario reportó tres síntomas a la vez: Stardew Valley dejó de arrancar, Pokémon Rubí Omega (Citra) pasó a un rendimiento inaceptable sin glitches y Dusklight quedó en pantalla negra aunque el proceso seguía vivo. El binario del shim no cambió en lógica (solo se tradujeron las cadenas de debug), así que la causa estaba en `bash-env.sh`:

- `export SDL_VIDEODRIVER=wayland` junto con `readonly SDL_VIDEODRIVER` choca con `mod_nuubOS.txt:15`, que exporta esa misma variable; `/run/nuubos/ports.log` mostró `mod_nuubOS.txt: line 15: SDL_VIDEODRIVER: readonly variable`. Es la causa directa de que Stardew no arranque.
- Forzar el backend SDL2 nativo a Wayland, añadir `SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY=1`/`SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0` y activar `force_s3tc_enable=true` correlacionó con la pantalla negra de Dusklight (`aurora::gfx::pipeline_cache: compiler thread could not bind the share context` → `glCreateShader failed`) y con la caída de rendimiento en Citra. No se han medido por separado, pero se retiran todos juntos por ser cambios nuevos del mismo despliegue.

**Corrección aplicada:** se mantiene únicamente `SDL3SHIM_SDL2_VIDEODRIVER=wayland` (necesario para el shim SDL3→SDL2), se deja `SDL_VIDEODRIVER` escribible para que lo fije PortMaster, y se eliminan las dos variables de escalado y `force_s3tc_enable`. El preload universal del shim se conserva: absorbe el antiguo `mono-gl-fix.so` (deepbind para Mono/LLVM, extensiones desktop-GL y `glGetTexImage`). El usuario confirmó que Dusklight y Rubí Omega volvieron a funcionar bien.

**Stardew Valley (segunda causa, ya cerrada):** tras el arreglo del entorno seguía sin arrancar. `/media/sd2/roms/ports/stardewvalley/log.txt` mostraba `SIGSEGV` con dos marcos del shim; al resolver las direcciones contra los símbolos del binario resultaron `shim_glCompressedTexImage2D` → `dxt_upload` (instrucción `ldrh [x1,#8]` del decodificador DXT). MonoGame llama a `glCompressedTexImage2D` con **`data == NULL`** para reservar almacenamiento de una textura comprimida; el shim intentaba decodificar bloques desde `NULL`. Corrección: reenviar al driver cuando `data == NULL` (también en `glCompressedTexSubImage2D`) y añadir un guard `if (!data) return NULL;` en `shim_dxt_decode`. Aprovechando el cambio se tradujo la última traza en español (`DXT: insufficient imageSize`). Verificado en dispositivo con `nuubos-portsctl launch StardewValley.sh`: `mono` sigue vivo tras ~90 s, sin `SIGSEGV` y log sin la native crash report. Sesión de prueba cerrada (`state=idle`). md5 actualizados en §"Artefactos desplegados".

## Investigación del glitch visual de Stardew (2026-11-26, en curso)

El usuario pidió no comparar capturas a tiempos fijos porque dispara la escena del bus manualmente y el momento varía. Se retiró el volcado de frames (`NUUBOS_FRAME_DUMP`) y `NUUBOS_SHIM_NO_MIPMAPS` del código y del entorno; `bash-env.sh` se restauró desde `bash-env-good-before-debug.sh` y solo conserva temporalmente `NUUBOS_SHIM_DEBUG=1` (backup `bash-env-good-before-trace.sh`). Las estructuras de frame/diagnóstico efímeras ya no existen en el binario desplegado.

Trazas de arranque (sin escena del bus): los filtros min de textura son `0x2601` (`GL_LINEAR`) o `0x2600` (`GL_NEAREST`); aparece `GL_TEXTURE_MAX_LEVEL` (`0x813D`) con valor `0x3E8` (1000) en varias texturas, pero **no hay filtros mipmap ni llamadas a `glGenerateMipmap`** en ese tramo (la traza estaba limitada a las 60 primeras llamadas, así que no descartaba cambios posteriores). Se detectó un defecto real y latente en la emulación de `GL_GENERATE_MIPMAP`: la condición `ff_has_mipmap_id(0)` de `shim_glTexImage2D` siempre era falsa (la función excluye `id == 0`), así que la generación diferida nunca se ejecutaba.

El binario desplegado añadía trazas acotadas (máx. 40-60 entradas por categoría) de `texparam`, `texupload` (niveles > 0), `generatemipmap`, `viewport`, `scissor` y `bindfbo`, todas bajo `NUUBOS_SHIM_DEBUG`. La ejecución en la que el usuario provocó la escena del bus solo produjo 104 líneas `texparam` (60 de filtro min y 44 de `GL_TEXTURE_MAX_LEVEL`), sin `viewport`, `scissor`, `bindfbo` ni `generatemipmap`: esas envolturas no ven las llamadas internas de gl4es y el tope de 60 ocultaba cambios tardíos. Por eso la traza se cambió a cambios de estado por textura con marca de tiempo, sin límites por número de llamadas.

## Revisión de renderizado software/CPU y quads (2026-11-27)

A petición del usuario se revisaron tres hipótesis, con estas decisiones:

- **No se fuerzan `GALLIUM_DRIVER=panfrost`, `LIBGL_ALWAYS_SOFTWARE=0` ni `PIPE_SHADER_DISK_CACHE=1` desde el shim.** La spec v2.1 fija `setenv("force_s3tc_enable", "true", 0)` en el paso 3.0.2 y es inmutable; cambiar el `0` por `1` la violaría, y forzar variables globales ya produjo la regresión de Dusklight/Citra del 26-11. En su lugar `detect_features()` lee `GL_RENDERER` y emite `renderer: ... [SOFTWARE]` bajo debug si aparece `llvmpipe`/`swrast`/`softpipe`/`software`; el H700 informa `Mali-G31 (Panfrost)` y `s3tc=1`, así que no hay degradación silenciosa ni decodificación DXT en CPU.
- **La ruta DXT nativa ya se prefiere** cuando `has_s3tc` es true; el decodificador CPU queda como fallback y ahora avisa una vez por categoría bajo debug (`DXT: CPU fallback ...`). `has_bgra8888` también se lee de la lista real de extensiones (`bgra=1`). El swizzle CPU de BGRA con `pixels != NULL` se mantiene porque es el modo por defecto exigido por la spec (Módulo E, Apéndice B); el modo `ext` sigue siendo opt-in y no se ha validado que BGRA sea renderizable para `glGetTexImage`.
- **Dos bugs reales en la conversión de `GL_QUADS`**: `quad_ibo_cap` guardaba un número de índices pero se comparaba con bytes, así que `glBufferData(GL_STREAM_DRAW)` se repetía en cada lote; y `shim_glDrawElements` pedía dos veces `scratch()`, con lo que el segundo `realloc` podía dejar colgando el puntero de índices de entrada y subir índices aleatorios. Ambos explican sprites/cuadrantes que aparecen tarde o parpadean. Corregidos: capacidad en bytes y una sola reserva con entrada y salida en el mismo bloque TLS.
- **Corregido el guard de `GL_GENERATE_MIPMAP`** descrito arriba: ahora se genera para la textura realmente enlazada, tal como exige el Módulo E.

Compilado con `gl-nuubos-shim-rebuild` y desplegado en el dispositivo (md5 en §"Artefactos desplegados"). Durante la revisión del propio parche se corrigió un tercer fallo: la expansión de índices `GL_UNSIGNED_BYTE`/`GL_UNSIGNED_SHORT` a `GL_UNSIGNED_INT` ensanchaba los datos **sobre el mismo buffer TLS que se estaba leyendo**, corrompiendo las entradas aún no consumidas. Ahora la reserva TLS única tiene tres regiones (triángulos de salida, índices ensanchados, índices originales). Tras la cualificación del usuario el glitch **persistió** con el fix de quads (2026-11-27). La corrida de 318 líneas no muestra `generatemipmap`, `viewport`, `scissor`, `bindfbo`, `DXT: CPU fallback` ni `SIGSEGV`; solo cambios de `GL_TEXTURE_MIN_FILTER` (`0x2601`↔`0x2600`) y `GL_TEXTURE_MAX_LEVEL=1000` en atlas grandes (hasta 2186x1230). A/B acotado al launcher de Stardew con `LIBGL_MIPMAP=3` (gl4es ignora mipmaps): el usuario provocó la escena y **el glitch sigue igual**; se revirtió el launcher desde `StardewValley.sh.bak-mipmap3`. El binario actual añade trazas acotadas (200) de `texsub` y del modo BGRA en `shim_init`. Arranque con `NUUBOS_SHIM_BGRA_MODE=ext`: `bgra=ext`, `renderer: Mali-G31 MC1 (Panfrost)`, `sw=0`; las primeras 43 subidas son `fmt=0x1908` (`GL_RGBA`), no BGRA. El usuario provocó la escena también con `NUUBOS_SHIM_BGRA_MODE=ext`: 190 `texsub`, **todas** `fmt=0x1908` (`GL_RGBA`) y a desplazamiento `(0,0)`; cero BGRA, cero DXT por CPU, cero mipmaps, cero software y cero `SIGSEGV`. El glitch sigue igual. A/B adicional: launcher modificado para **no usar gl4es** (`unset SDL_VIDEO_GL_DRIVER`/`SDL_VIDEO_EGL_DRIVER`, `LD_LIBRARY_PATH` sin `gl4es`, `NUUBOS_SHIM_DESKTOP_GL=1`): el log ya no contiene `dlopen` de `.../gl4es/libGL.so.1`, sino `libGLESv2.so.2` directo con `desktop=1`; 184 `texsub` también `GL_RGBA`; el usuario provocó la escena y **el glitch es idéntico**. Por tanto quedan descartados gl4es, mipmaps, BGRA, DXT-CPU, fallback software y las tres correcciones de quads: el defecto es anterior al shim actual y no se ha podido aislar más sin instrumentación de draws/shaders dentro de MonoGame. Se restauró `StardewValley.sh` original, se borraron los backups temporales del launcher y se quitó `NUUBOS_SHIM_DEBUG=1` de `/usr/lib/nuubos/ports/bash-env.sh`; sesión `state=idle`. En ese momento no había commit ni push.

## Cierre de la investigación del glitch (2026-11-27)

Tras los A/B de la sección anterior se añadió instrumentación temporal bajo `NUUBOS_SHIM_DEBUG` para correlacionar el defecto con texturas, draws y shaders reales, siempre disparados manualmente por el usuario (sin números de frame ni esperas fijas):

- **Drawtrace** (`draw[]`, `glsym:`): 98 símbolos GL resueltos por el shim. Los draws reales pasan por `shim_glDrawElements` (`mode=0x4`, `type=0x1403`, `prog=1`, `unit=0x84c0`). La escena del bus usa atlas grandes (textura 57 de 1200x800), dos render targets de 1366x1025 (texturas 47/48/50/51) y batches de `count=144/198/36/96`; el blit final a `640x480` usa la textura 50. No aparecieron `viewport:`, `scissor:`, `bindfbo:` ni `generatemipmap:` en las corridas; los cambios de `GL_TEXTURE_MIN_FILTER` y `GL_TEXTURE_MAX_LEVEL=1000` son rutina.
- **A/B `NUUBOS_SHIM_CLAMP_ALL=1`**: MonoGame nunca llama a `glTexParameteri(GL_TEXTURE_WRAP_S/T)`, así que las texturas conservan `GL_REPEAT`. Se aplicó `GL_CLAMP_TO_EDGE` al crear cada textura (el wrapper directo no surtía efecto). El usuario provocó la escena: **sin cambio**. Retirado del dispositivo.
- **A/B de precisión de shader**: el fragment shader de MojoShader declara `precision mediump float` (`GL_FRAGMENT_SHADER`, 352 bytes). Se promovió `mediump`→`highp` en todos los calificadores antes de `glShaderSource` (352→348 bytes, verificado en el log). El usuario provocó la escena: **sin cambio**. Retirado.
- **Captura puntual**: `/dev/fb0` está en modo `640x480` pero labwc compone en Wayland y la lectura devuelve un color plano. Se implementó un volcado PPM desde el shim disparado por el blit final (forma de draw + viewport, sin relojes), primero del framebuffer por defecto (plano `(5,3,4)`) y después de la textura RTT vía FBO (`tex=50`, 1366x1025, correcto). El sandbox bwrap tiene `/tmp` como tmpfs privado, así que la ruta se movió a `/roms/ports/stardewvalley/`. **Limitación asumida**: el modelo de análisis no puede interpretar la imagen y no se dispone de visor en el dispositivo, así que no se pudo usar para diagnosticar el artefacto; se retiró la captura del código y del dispositivo.

**Conclusión**: el glitch de la escena del bus es una **limitación conocida y aceptada por el usuario**, no una regresión del shim. Quedan descartados gl4es, mipmaps, BGRA, DXT por CPU, fallback software, wrap mode, precisión de shader y las correcciones de quads. No se seguirá instrumentando ni probando a ciegas.

### Última pasada de bugs y optimizaciones (misma sesión)

Se retiró toda la instrumentación temporal (captura PPM, `NUUBOS_SHIM_CLAMP_ALL`, promoción `highp`, `glsym:`/`draw[]`/trazas de shader) y se corrigieron defectos reales encontrados durante la revisión:

1. **`glTexParameteriv`/`glTexParameterfv`**: solo `GL_TEXTURE_BORDER_COLOR` (`0x1004`) toma cuatro valores; el código trataba todo `pname` como vector, leyendo `params[1..3]` de un buffer de un solo entero y aplicando el valor dos veces (wrapper + driver). Ahora el caso vectorial va directo al driver y el resto usa `params[0]`.
2. **Swizzle de texturas**: `set_unpack_defaults()` se llamaba **antes** de `swizzle_upload()`, de modo que se perdía `GL_UNPACK_ROW_LENGTH`/`SKIP_*`/`ALIGNMENT` y un PBO enlazado se interpretaba como puntero de CPU. Ahora se swizzlea primero con el estado original y se reinician los defaults solo para la subida convertida. Si el swizzle falla y el driver anuncia BGRA nativa se reenvían los bytes originales; en el resto de casos se mantiene `NULL` (asignación indefinida) en vez de etiquetar BGRA como RGBA.
3. **`drain_errors_return()`**: devolvía el último error de la cola aunque documentaba devolver el primero; ahora conserva el primero mientras drena el resto.
4. **`ensure_scratch_key()`**: `g_scratch_key_ready` se comprobaba sin sincronización desde varios hilos SDL/Mesa; se sustituye por `pthread_once` (ruta rápida atómica, sin carrera).
5. **`scratch()`**: se añade un guard de desbordamiento en el duplicado de capacidad (`cap > SIZE_MAX/2`) antes de `realloc`.
6. **Optimizaciones**: `shim_get_ctx()` se diferencia en las rutas de `glTexImage2D`/`glTexSubImage2D` (solo se busca cuando hace falta BGRA o el fallback), evitando la búsqueda de contexto en la subida RGBA habitual; `quad_ibo_cap` se mide en bytes para no reasignar el IBO en cada lote (fix de la sección anterior).

Compilado con `gl-nuubos-shim-rebuild` (sin warnings), desplegado y verificado con `NUUBOS_SHIM_DEBUG` retirado: `state=running`, proceso `mono ../SVLoader.exe Stardew Valley.exe` activo y log sin trazas. `bash-env.sh` restaurado desde `bash-env.sh.bak-drawtrace` (sin `NUUBOS_SHIM_DEBUG` ni `NUUBOS_SHIM_CAPTURE`); el PPM temporal se borró del dispositivo. **Commit/push autorizados por el usuario y ejecutados en el cierre de la sesión.**

## Resultado final — `FINAL`

**Declarado `FINAL` para el alcance auditable de la spec v2.1.** Shim compilado, desplegado y verificado en aarch64 y armhf; G.2 completo en lo cubierto por la spec, G.3 funcional en el subconjunto opt-in (decisión cerrada), Módulo H implementado (incluidas listas de visualización y alias), y Mesa/DRI de 32 bit integrado y validado con un contexto EGL/GLES real (coste aceptado). La auditoría de cobertura (259 tokens de la spec frente a 329 entradas del shim) no deja funciones reales, exportaciones solo-escritorio ni alias ARB/EXT sin cubrir; los 27 residuales son familias genéricas o exclusiones explícitas de la propia spec. Los parches `0012`/`0013` se conservan por decisión del usuario, sin reclamar beneficio medible. El glitch de la escena del bus queda como limitación aceptada (§"Cierre de la investigación del glitch"). El usuario autorizó expresamente el commit y el push de este cierre el 2026-11-27.
