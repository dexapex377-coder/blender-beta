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
| `actions` | emite catálogo base para construir menús en Compose |
| `menu.tree` | emite la lista de **todos** los menús detectados |
| `menu:<id>` | emite los items de un menú/submenú con sus comandos |
| `menu.open:<id>` | fallback: abre el menú nativo |
| `mesh.subdivide[:<n>]` | subdivide en Edit Mode (`number_cuts`) |
| `subscribe` | emite `state` cuando cambia selección/modo/workspace/tool |
| `event:<nombre>:<json>` | emite un evento arbitrario |
| `file.new` / `file.open:<path>` | archivo |
| `workspace.set:<name>` | cambia workspace |
| `mode.set:<MODE>` | cambia modo (`EDIT`, `OBJECT`, `SCULPT`…) |
| `tool.set:<idname>` | tool con override de `VIEW_3D` |
| `tool.activate:<idname>` | activa tool y emite `tool_settings` |
| `tool.gizmo:<id>:<prop>[\|json]` | lee/escribe prop del gizmo del tool |
| `tool.opprop:<id>:<prop>[\|json]` | lee/escribe prop del operador del tool |
| `tool.exec:<op>[\|json]` | ejecuta el operador del tool |
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

## Caso concreto: Subdivide y menús/submenús

Los menús son clases Python cuyo `draw()` llama a
`layout.operator("mesh.subdivide")` o `layout.menu("OTRO_MENU")`. El bridge
parsea `scripts/startup/bl_ui/*.py` y devuelve el árbol real:

```kotlin
b("menu.tree")                       // evento menu_tree
b("menu:VIEW3D_MT_edit_mesh_edges")  // evento menu_items con sus items
```

Cada item trae:

```json
{"type":"operator","id":"mesh.subdivide","label":"Subdivide",
 "command":"op:bpy.ops.mesh.subdivide@VIEW_3D"}
```

Al tocar Subdivide en tu menú Compose:

```kotlin
b("mode.set:EDIT")
b("mesh.subdivide:2")
```

No llamás al menú: llamás **al operador que el menú llama**. Igual para
cualquier menú/submenú, tool o panel.

## Caso concreto: tool Extrude

El tool es `builtin.extrude_region`, su operador es
`view3d.edit_mesh_extrude_move_normal` y su ajuste es `axis_type`
(`NORMAL` / `XYZ`) del gizmo `VIEW3D_GGT_xform_extrude`.

```kotlin
b("mode.set:EDIT")
b("tool.activate:builtin.extrude_region")
b("tool.gizmo:VIEW3D_GGT_xform_extrude:axis_type|\"XYZ\"")
b("tool.exec:view3d.edit_mesh_extrude_move_normal|{\"release_confirm\":true}")
```

Compose lee `tool_settings` / `tool_setting` desde `events.jsonl`: son las
mismas props del tool nativo, aunque la UI de Blender esté oculta.

## Eventos (Blender -> Kotlin)

`emit()` escribe una línea JSON por evento en `events.jsonl`. Kotlin pollea el
archivo y mantiene su UI sincronizada. Eventos: `pong`, `log`, `state`,
`actions`, `op_result`, `set_result`, `get_result` y los propios.

## Coste

El primer build de 5.3 sí necesita `libblender.so` + APK por el `drain` en
`wm.cc`. Después, toda la UI se itera en Python y Compose sin tocar C++.
