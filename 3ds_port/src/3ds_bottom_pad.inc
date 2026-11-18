/* Part of 3ds_port's bottom screen (3ds_bottom_ui.c): the arrow the lists draw and the Berry Blender's button (included before 3ds_bottom_fieldmenu.inc, which uses the arrow). */

/*
 * A screen of the game the bottom screen does not draw runs on the top screen
 * as it always did; the bottom one shows only its backdrop (the physical keys
 * already do what a touch pad would). What stays here: the hit id the Berry
 * Blender's button uses (HIT_PAD_A) and the arrow the lists draw (PadArrow).
 */
enum
{
    HIT_PAD_A = HIT_ACTION + 4
};

/* An arrow of the cross: a triangle of rows, pointing dir (0 up, 1 down, 2 left, 3 right). */
static void PadArrow(int cx, int cy, int dir, u16 c)
{
    for (int i = 0; i < 9; ++i)
    {
        int w = 2 * i + 1;

        if (dir == 0)
            FillRect(cx - i, cy - 8 + i, w, 1, c);
        else if (dir == 1)
            FillRect(cx - i, cy + 8 - i, w, 1, c);
        else if (dir == 2)
            FillRect(cx - 8 + i, cy - i, 1, w, c);
        else
            FillRect(cx + 8 - i, cy - i, 1, w, c);
    }
}

/* The Berry Blender's big button is the A key, held while a finger is on it. */
static u16 PadKeysHeld(bool8 touching, int x, int y)
{
    if (!touching || HitTest(x, y) != HIT_PAD_A)
        return 0;
    return A_BUTTON;
}
