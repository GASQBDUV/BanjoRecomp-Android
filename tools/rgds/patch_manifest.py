"""Apply the swapped-layout activities to an apktool-decoded AndroidManifest.xml.

Mirrors the source manifest (android/app/src/main/AndroidManifest.xml): the
launcher intent moves from BanjoSDLActivity to BanjoLauncherActivity, and
DualScreenStatsActivity is added.
"""
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

game = re.search(r'<activity[^>]*android:name="io\.github\.banjorecomp\.BanjoSDLActivity".*?</activity>', text, re.S)
if not game:
    sys.exit("BanjoSDLActivity not found in manifest")
block = game.group(0)
patched = re.sub(r'\s*<intent-filter>.*?android\.intent\.category\.LAUNCHER.*?</intent-filter>', '', block, flags=re.S)
if patched == block:
    sys.exit("launcher intent-filter not found on BanjoSDLActivity")
text = text.replace(block, patched)

extra = """        <activity android:excludeFromRecents="true" android:exported="true" android:label="Banjo Recompiled" android:name="io.github.banjorecomp.BanjoLauncherActivity" android:noHistory="true" android:theme="@android:style/Theme.Translucent.NoTitleBar">
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
        <activity android:configChanges="keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|smallestScreenSize|uiMode" android:excludeFromRecents="true" android:exported="false" android:name="io.github.banjorecomp.DualScreenStatsActivity" android:theme="@style/BanjoTheme"/>
"""
if text.count("</application>") != 1:
    sys.exit("unexpected manifest shape")
text = re.sub(r"(\s*)</application>", lambda m: "\n" + extra + "    </application>", text, count=1)
open(path, "w", encoding="utf-8").write(text)
print("manifest patched")
