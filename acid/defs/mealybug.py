from test import *


def dmg(name):
    result = name.replace(".gb", "_dmg_blob.png")
    return Test(
        "mealybug-tearoom-tests/" + name + " (DMG)",
        rom="mealybug-tearoom-tests/" + name,
        result="mealybug-tearoom-tests/" + result,
        model=DMG,
        runtime=0.5
    )

def cgbc(name):
    result = name.replace(".gb", "_cgb_c.png")
    return Test(
        "mealybug-tearoom-tests/" + name + " (CGB RevC)",
        rom="mealybug-tearoom-tests/" + name,
        result="mealybug-tearoom-tests/" + result,
        model=CGB,
        runtime=0.5
    )

def cgbd(name):
    result = name.replace(".gb", "_cgb_d.png")
    return Test(
        "mealybug-tearoom-tests/" + name + " (CGB RevD)",
        rom="mealybug-tearoom-tests/" + name,
        result="mealybug-tearoom-tests/" + result,
        model=CGB,
        runtime=0.5
    )

dmgs = [
    dmg("ppu/m2_win_en_toggle.gb"),
    dmg("ppu/m3_bgp_change.gb"),
    dmg("ppu/m3_bgp_change_sprites.gb"),
    dmg("ppu/m3_lcdc_bg_en_change.gb"),
    # dmgb("ppu/m3_lcdc_bg_en_change2.gb"),
    dmg("ppu/m3_lcdc_bg_map_change.gb"),
    # dmgb("ppu/m3_lcdc_bg_map_change2.gb"),
    dmg("ppu/m3_lcdc_obj_en_change.gb"),
    dmg("ppu/m3_lcdc_obj_en_change_variant.gb"),
    dmg("ppu/m3_lcdc_obj_size_change.gb"),
    dmg("ppu/m3_lcdc_obj_size_change_scx.gb"),
    dmg("ppu/m3_lcdc_tile_sel_change.gb"),
    # dmgb("ppu/m3_lcdc_tile_sel_change2.gb"),
    dmg("ppu/m3_lcdc_tile_sel_win_change.gb"),
    # dmgb("ppu/m3_lcdc_tile_sel_win_change2.gb"),
    dmg("ppu/m3_lcdc_win_en_change_multiple.gb"),
    dmg("ppu/m3_lcdc_win_en_change_multiple_wx.gb"),
    dmg("ppu/m3_lcdc_win_map_change.gb"),
    # dmgb("ppu/m3_lcdc_win_map_change2.gb"),
    dmg("ppu/m3_obp0_change.gb"),
    dmg("ppu/m3_scx_high_5_bits.gb"),
    # dmgb("ppu/m3_scx_high_5_bits_change2.gb"),
    dmg("ppu/m3_scx_low_3_bits.gb"),
    dmg("ppu/m3_scy_change.gb"),
    # dmgb("ppu/m3_scy_change2.gb"),
    dmg("ppu/m3_window_timing.gb"),
    dmg("ppu/m3_window_timing_wx_0.gb"),
    dmg("ppu/m3_wx_4_change.gb"),
    dmg("ppu/m3_wx_4_change_sprites.gb"),
    dmg("ppu/m3_wx_5_change.gb"),
    dmg("ppu/m3_wx_6_change.gb"),
    # dmgb("ppu/win_without_bg.gb"),
]

cgbcs = [
    cgbc("ppu/m2_win_en_toggle.gb"),
    cgbc("ppu/m3_bgp_change.gb"),
    cgbc("ppu/m3_bgp_change_sprites.gb"),
    cgbc("ppu/m3_lcdc_bg_en_change.gb"),
    cgbc("ppu/m3_lcdc_bg_en_change2.gb"),
    cgbc("ppu/m3_lcdc_bg_map_change.gb"),
    cgbc("ppu/m3_lcdc_bg_map_change2.gb"),
    cgbc("ppu/m3_lcdc_obj_en_change.gb"),
    cgbc("ppu/m3_lcdc_obj_en_change_variant.gb"),
    cgbc("ppu/m3_lcdc_obj_size_change.gb"),
    cgbc("ppu/m3_lcdc_obj_size_change_scx.gb"),
    cgbc("ppu/m3_lcdc_tile_sel_change.gb"),
    cgbc("ppu/m3_lcdc_tile_sel_change2.gb"),
    cgbc("ppu/m3_lcdc_tile_sel_win_change.gb"),
    cgbc("ppu/m3_lcdc_tile_sel_win_change2.gb"),
    cgbc("ppu/m3_lcdc_win_en_change_multiple.gb"),
    # cgbc("ppu/m3_lcdc_win_en_change_multiple_wx.gb"),
    cgbc("ppu/m3_lcdc_win_map_change.gb"),
    cgbc("ppu/m3_lcdc_win_map_change2.gb"),
    cgbc("ppu/m3_obp0_change.gb"),
    cgbc("ppu/m3_scx_high_5_bits.gb"),
    cgbc("ppu/m3_scx_high_5_bits_change2.gb"),
    cgbc("ppu/m3_scx_low_3_bits.gb"),
    cgbc("ppu/m3_scy_change.gb"),
    cgbc("ppu/m3_scy_change2.gb"),
    cgbc("ppu/m3_window_timing.gb"),
    cgbc("ppu/m3_window_timing_wx_0.gb"),
    # cgbc("ppu/m3_wx_4_change.gb"),
    cgbc("ppu/m3_wx_4_change_sprites.gb"),
    # cgbc("ppu/m3_wx_5_change.gb"),
    # cgbc("ppu/m3_wx_6_change.gb"),
    # cgbc("ppu/win_without_bg.gb"),
]

cgbds = [
    cgbd("ppu/m2_win_en_toggle.gb"),
    cgbd("ppu/m3_bgp_change.gb"),
    cgbd("ppu/m3_bgp_change_sprites.gb"),
    cgbd("ppu/m3_lcdc_bg_en_change.gb"),
    # cgbd("ppu/m3_lcdc_bg_en_change2.gb"),
    cgbd("ppu/m3_lcdc_bg_map_change.gb"),
    # cgbd("ppu/m3_lcdc_bg_map_change2.gb"),
    cgbd("ppu/m3_lcdc_obj_en_change.gb"),
    cgbd("ppu/m3_lcdc_obj_en_change_variant.gb"),
    cgbd("ppu/m3_lcdc_obj_size_change.gb"),
    cgbd("ppu/m3_lcdc_obj_size_change_scx.gb"),
    cgbd("ppu/m3_lcdc_tile_sel_change.gb"),
    # cgbd("ppu/m3_lcdc_tile_sel_change2.gb"),
    cgbd("ppu/m3_lcdc_tile_sel_win_change.gb"),
    # cgbd("ppu/m3_lcdc_tile_sel_win_change2.gb"),
    cgbd("ppu/m3_lcdc_win_en_change_multiple.gb"),
    # cgbd("ppu/m3_lcdc_win_en_change_multiple_wx.gb"),
    cgbd("ppu/m3_lcdc_win_map_change.gb"),
    # cgbd("ppu/m3_lcdc_win_map_change2.gb"),
    cgbd("ppu/m3_obp0_change.gb"),
    cgbd("ppu/m3_scx_high_5_bits.gb"),
    # cgbd("ppu/m3_scx_high_5_bits_change2.gb"),
    cgbd("ppu/m3_scx_low_3_bits.gb"),
    cgbd("ppu/m3_scy_change.gb"),
    # cgbd("ppu/m3_scy_change2.gb"),
    cgbd("ppu/m3_window_timing.gb"),
    cgbd("ppu/m3_window_timing_wx_0.gb"),
    # cgbd("ppu/m3_wx_4_change.gb"),
    cgbd("ppu/m3_wx_4_change_sprites.gb"),
    # cgbd("ppu/m3_wx_5_change.gb"),
    # cgbd("ppu/m3_wx_6_change.gb"),
    # cgbd("ppu/win_without_bg.gb"),
]

all = dmgs # + cgbcs + cgbds
