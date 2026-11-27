/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.kprofiles;

import android.graphics.drawable.Icon;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings tile: a tap moves to the next profile. */
public class KProfilesTileService extends TileService {

    @Override
    public void onStartListening() {
        super.onStartListening();
        updateTile(Profiles.get());
    }

    @Override
    public void onClick() {
        super.onClick();
        String mode = Profiles.next(Profiles.get());
        if (!Profiles.set(mode)) mode = Profiles.get();
        updateTile(mode);
    }

    private void updateTile(String mode) {
        Tile tile = getQsTile();
        if (tile == null) return;
        tile.setLabel(getString(R.string.tile_label));
        tile.setSubtitle(getString(Profiles.titleRes(mode)));
        tile.setIcon(Icon.createWithResource(this, Profiles.iconRes(mode)));
        // Balanced is the default, the other two stand out
        tile.setState(Profiles.BALANCED.equals(mode) ? Tile.STATE_INACTIVE : Tile.STATE_ACTIVE);
        tile.updateTile();
    }
}
