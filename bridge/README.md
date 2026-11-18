# Puente UI (5.3 beta / kb)

Un solo punto de entrada JNI para toda la UI Compose:

```kotlin
BlenderActivity.dispatchBridgeStatic("workspace.set:Sculpting")
```

El hilo de la UI **solo encola** la cadena (`JNI -> blenderQueueBridgeCommand`).
El `drain` vive en `source/blender/windowmanager/intern/wm.cc`, dentro de
`WM_main_loop_body()`, con un `bContext` válido, y ejecuta
`obl_bridge.handle(cmd)`.

## Archivo editable en el dispositivo

```
/sdcard/Android/data/com.kronos3d.app/files/obl/obl_bridge.py
```

Se puede editar con `adb push`; no hace falta volver a compilar la APK ni
`libblender.so`. Cada dispatch recarga el módulo, así que los cambios se ven
en el siguiente toque.

## Protocolo

Igual que en 3.6.22:

| Comando | Acción |
|---|---|
| `ping` | comprueba que el puente responde |
| `bridge.reload` | recarga el módulo Python |
| `file.new` | `bpy.ops.wm.read_homefile(app_template="")` |
| `file.open:<path>` | abre un `.blend` |
| `workspace.set:<name>` | cambia `window.workspace` |
| `tool.set:<idname>` | `bpy.ops.wm.tool_set_by_id` con override de `VIEW_3D` |
| `ui.hide` | oculta topbar, statusbar, headers, toolbar y sidebar |
| `ui.restore` | restaura lo oculto por `ui.hide` |
| `op:<python>` | ejecuta Python arbitrario (canal de desarrollo) |

Ejemplos:

```kotlin
BlenderActivity.dispatchBridgeStatic("file.new")
BlenderActivity.dispatchBridgeStatic("workspace.set:Shading")
BlenderActivity.dispatchBridgeStatic("tool.set:builtin_brush.Clay")
BlenderActivity.dispatchBridgeStatic("ui.hide")
```

## Instalación del módulo

```bash
adb push obl_bridge.py /sdcard/Android/data/com.kronos3d.app/files/obl/obl_bridge.py
```

Verificar en logcat (tag `blender`):

```
[OBL.BRIDGE] JNI queued: ...
[OBL.BRIDGE] dispatch: ...
[OBL.BRIDGE] ...
```

## Notas de build

A diferencia de 3.6, aquí el loop está en Blender (`wm.cc`), así que este
puente **sí requiere un rebuild de `libblender.so`** la primera vez. Después,
cada acción nueva es solo Python y no vuelve a tocar C++.
