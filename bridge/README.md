# Puente UI v2 (5.3 beta / kb)

Un solo JNI (`nativeDispatch`) para toda la UI Compose. JNI encola en GHOST;
`WM_main_loop_body()` drena en el hilo de Blender con `bContext` válido y llama
a `obl_bridge.handle(cmd)`.

## Archivo editable

```
/sdcard/Android/data/com.kronos3d.app/files/obl/obl_bridge.py
/sdcard/Android/data/com.kronos3d.app/files/obl/events.jsonl
```

El módulo se recarga en cada dispatch. Cambiarlo no requiere rebuild de
`libblender.so` ni de la APK.

## Comandos (Kotlin -> Blender)

| Comando | Acción |
|---|---|
| `ping` | emite `pong` |
| `bridge.reload` | recarga el módulo |
| `state` | emite snapshot: workspace, area, mode, tool, objeto activo, selección |
| `actions` | emite catálogo para construir menús en Compose |
| `subscribe` | emite `state` cuando cambia selección/modo/workspace/tool |
| `event:<nombre>:<json>` | emite un evento arbitrario |
| `file.new` / `file.open:<path>` | archivo |
| `workspace.set:<name>` | cambia workspace |
| `tool.set:<idname>` | tool con override de `VIEW_3D` |
| `op:<idname>[\|json]@AREA` | operador con kwargs y contexto de editor |
| `set:<path>\|<json>[@AREA]` | escribe una propiedad RNA |
| `get:<path>` | lee una propiedad RNA y emite `get_result` |
| `ui.hide` / `ui.restore` | oculta/restaura la UI nativa |
| `python:<code>` | canal de desarrollo, Python arbitrario |

`AREA` puede ser `VIEW_3D`, `OUTLINER`, `PROPERTIES`, `NODE_EDITOR`,
`SEQUENCE_EDITOR`, `IMAGE_EDITOR`, `TEXT_EDITOR`, etc. El puente aplica
`temp_override(window, area, region)` sobre esa editor, así los operadores de
Outliner, Properties o Tool Settings reciben el contexto correcto.

Ejemplos:

```kotlin
val b = BlenderActivity::dispatchBridgeStatic

b("workspace.set:Shading")
b("op:bpy.ops.outliner.item_activate|{\"extend\":false}@OUTLINER")
b("set:bpy.context.scene.render.resolution_x|1920")
b("get:bpy.context.object.name")
b("subscribe")
b("ui.hide")
```

## Eventos (Blender -> Kotlin)

`emit()` escribe una línea JSON por evento en `events.jsonl`. Kotlin pollea el
archivo y mantiene su UI sincronizada. Eventos: `pong`, `log`, `state`,
`actions`, `op_result`, `set_result`, `get_result` y los propios.

## Coste

El primer build de 5.3 sí necesita `libblender.so` + APK por el `drain` en
`wm.cc`. Después, toda la UI se itera en Python y Compose sin tocar C++.
