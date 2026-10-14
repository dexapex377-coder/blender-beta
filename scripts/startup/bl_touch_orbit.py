# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""One finger drag orbits the 3D viewport where there is nothing to grab.

Registered here rather than from an add-on so the keymap item in
blender_default.py finds it without the user having to install anything.

The item that calls this lives in the generic "3D View" keymap only, and that is
what decides the whole behaviour: the "3D View Tool: ..." keymaps are consulted
while a tool is active, so with a tool running there is no orbit entry to reach
and the tool wins. With none running the generic keymap is what gets asked. No
mode checks needed for that, and none are done -- Sculpt and 2D Animation reach
their own tool keymaps and are left alone.

What is left to decide is whether there is something under the finger, which is
what this operator's poll answers: a ray that misses means there is nothing to
select there, so the drag is free to mean orbit. A ray that hits returns False,
and WM_event_match_keymap_item() carries on to the next item, which is
view3d.select. One finger then selects what is under it and orbits what is not.

Gizmos need no handling here: a hovered manipulator is given the event before an
ordinary operator sees it.

Set /sdcard/Android/data/com.kronos3d.app/files/toque_orbitar.txt to "0" to turn
this off without a rebuild. The value is re-read periodically, not cached, so the
change takes effect while Blender is open.
"""

from __future__ import annotations

import os

import bpy
from bpy.app.handlers import persistent
from bpy_extras.view3d_utils import region_2d_to_vector_3d

TOUCH_FLAG_FILE = "/sdcard/Android/data/com.kronos3d.app/files/toque_orbitar.txt"

_enabled = True


def _read_flag() -> bool:
    """Re-read on every call rather than caching, so it can be flipped live."""
    try:
        with open(TOUCH_FLAG_FILE) as f:
            return f.read().strip() != "0"
    except OSError:
        return True


def _ray_misses(context) -> bool:
    region = context.region
    if region is None or region.type != 'WINDOW':
        return False
    rv3d = region.rv3d
    if rv3d is None or context.region_data is None:
        return False
    try:
        direction = region_2d_to_vector_3d(region, context.region_data,
                                           (context.mouse_x, context.mouse_y))
        direction.normalize()
        depsgraph = context.evaluated_depsgraph_get()
        hit, _location, _normal, _index, _object, _matrix = context.scene.ray_cast(
            depsgraph, rv3d.view_location, direction)
    except Exception:
        # Not knowing is the same as something being there: not orbiting when the
        # answer is unavailable leaves the normal gesture intact.
        return False
    return not hit


class VIEW3D_OT_touch_orbit(bpy.types.Operator):
    bl_idname = "view3d.touch_orbit_empty"
    bl_label = "Orbit (Empty Space)"
    bl_description = "One finger drag orbits the view, but only where there is nothing to select"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        if not _read_flag():
            return False
        if context.mode != 'OBJECT':
            return False
        return _ray_misses(context)

    def invoke(self, context, event):
        return bpy.ops.view3d.view_orbit('INVOKE_DEFAULT', type='ORBITLEFT')


class VIEW3D_OT_touch_no_tool(bpy.types.Operator):
    """Drop the active tool, so a plain finger drag is free to orbit.

    Blender has no way to reach this from the interface: the toolbar always has
    something selected. Setting it to NONE is what makes the generic keymap the
    one consulted, which is what turns orbit on for this area.
    """
    bl_idname = "view3d.touch_no_tool"
    bl_label = "Clear Tool (Touch Orbit)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and context.space_data is not None

    def execute(self, context):
        for screen in bpy.data.screens:
            for area in screen.areas:
                if area.type == 'VIEW_3D':
                    area.spaces.active.tool_settings.tool = 'NONE'
        # The toolbar highlights whatever it thinks is running; a redraw is the
        # only way it notices the change.
        for area in context.screen.areas:
            area.tag_redraw()
        self.report({'INFO'}, "Tool cleared")
        return {'FINISHED'}


_classes = (VIEW3D_OT_touch_orbit, VIEW3D_OT_touch_no_tool)


def register():
    for cls in _classes:
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            bpy.utils.unregister_class(cls)
            bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass


if __name__ == "__main__":
    register()
    print("bl_touch_orbit: orbitar con un dedo registrado", flush=True)