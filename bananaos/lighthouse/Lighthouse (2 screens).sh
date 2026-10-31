#!/bin/bash
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls
GAMEDIR=/$directory/ports/lighthouse
cd $GAMEDIR
. /usr/libexec/banana/one-panel   # the bottom panel is kept dark by the menu while a port runs; light it
one_panel_dark top      # (sets up the helper's state)
one_panel_restore
export BANANA_PANEL=1
LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH" ./Lighthouse > $GAMEDIR/panel.log 2>&1
