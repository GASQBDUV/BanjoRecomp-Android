#!/bin/bash
# Lighthouse (Banjo-Kazooie) with the companion screen on the bottom panel. bananaOS / Anbernic RG DS.
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
# the menu keeps the bottom panel dark while a port runs; light it again
. /usr/libexec/banana/one-panel
one_panel_dark top
one_panel_restore
export BANANA_PANEL=1
LD_LIBRARY_PATH="$GAMEDIR/libs-bananaos:$GAMEDIR/libs:$LD_LIBRARY_PATH" ./Lighthouse.bananaos > $GAMEDIR/lighthouse-bananaos.log 2>&1
