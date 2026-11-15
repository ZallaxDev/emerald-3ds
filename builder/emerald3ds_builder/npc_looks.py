"""New looks for the field's people: Facets' NPC sets, fetched and converted.

Facets, by ElvieBlooms (https://github.com/ElvieBlooms/facets, MIT for its
code, its art free to use with credit) draws every person of Ruby, Sapphire
and Emerald again in a DS style, one 32x32 sheet of nine frames each:

    DIEGO      DiegoWT's Black/White-style trainers and townsfolk (OW PACK v2)
    ZAFFRE     PurpleZaffre's named characters (gym leaders, Wallace, Archie,
               Maxie), from the Ultimate Gen 4 Overworlds Pack
    TAIGA      Taiga's ORAS-style trainers (Taiga Collection)
    COMMUNITY  KyleDove, Jext and Hyo, with some by DiegoWT and Aqua, for
               everyone else

One look is taken for each person, in that order of the sets. Nothing
fetched reaches the SD card as it is, and nothing of it is kept in this
repository: the files are fetched at the commit pinned below and checked
against the sizes and SHA-256 sums listed here.

Pack files:

    npcs/NAME.bin   NAME = the person's OBJ_EVENT_GFX_* name ("WOMAN_4")

A .bin (little endian):

    0   magic   b"NPL1"
    4   frames  u8, 9: the game's own order (down, up, left, then the walk)
    5   width   u8, 32
    6   height  u8, 32
    7   0       u8
    8   palette u16 x 16 (BGR555 as the GBA's, colour 0 transparent)
    40  tiles   frames x 512 bytes, each frame 16 8x8 4bpp tiles row by
                row, as an OBJ in the GBA's one-dimensional mapping

The player's looks: Emerald Lens, by ElvieBlooms (https://github.com/
ElvieBlooms/emerald_lens, MIT for its code, its looks shared with credit),
each one's overworld sheets for Ruby, Sapphire and Emerald:

    players/LOOK/SHEET.bin   LOOK in PLAYER_LOOKS (the option's order),
                             SHEET walk, bike, acroBike, surf, fieldMove,
                             underwater, fishing, watering, decorating;
                             back, the battle's back picture: four 64x64
                             frames (standing, then the throw), where the
                             look has one

as the people's files, with the sheet's own number of frames (the game's
for that picture: 18 walking and running, 27 on the Acro Bike...). A sheet
16 pixels wide is centred in 32.

The builder adds both to the data pack (build.py, an optional section like
the followers': a build that cannot fetch them still makes a working pack).
Run as a script (no package imports) for a development RomFS:
python npc_looks.py --out ROMFS_DIR [--sources FACETS_CLONE]
[--lens-sources LENS_CLONE] [--preview sheet.png]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import struct
import sys
import urllib.request

REPO = "ElvieBlooms/facets"
COMMIT = "b290b074e144aaeb0c9a04e4639f4a77e7c37b79"
URL = "https://raw.githubusercontent.com/{repo}/{commit}/{path}"
SETS = ("diego", "zaffre", "taiga", "community")
FRAMES = 9
SIZE = 32
MAGIC = b"NPL1"
PREFIX = "npcs/"
CREDIT = ("Facets by ElvieBlooms (MIT). People: DiegoWT (Black/White style), PurpleZaffre (named characters), "
          "Taiga (ORAS style), KyleDove, Jext and Hyo with some by DiegoWT and Aqua (everyone else). "
          "Free to use with credit; the characters belong to Nintendo, Game Freak and Creatures.")

LENS_REPO = "ElvieBlooms/emerald_lens"
LENS_COMMIT = "9a97879e5c82ea44e2acf7717fa94f864b382ceb"
PLAYER_PREFIX = "players/"
# The option's order (3ds_extras.c, "player_look"): its value 1 is the first.
PLAYER_LOOKS = ("brendan_hgss", "may_hgss", "brendan_oras", "may_oras", "ash_hgss", "kris_hgss",
                "gold_hgss", "silver_hgss", "green_hgss")
PLAYER_CREDIT = ("Emerald Lens by ElvieBlooms (MIT; looks shared with credit). HGSS Brendan and May by DiegoWT; "
                 "ORAS Brendan and May from Team Aqua's Asset Repo; HGSS Ash by PKMNTrainerRick; Kris by LouLilie; "
                 "HGSS Gold by GregarLink10; HGSS Silver by lichenprincess; HGSS Green compiled by ChromeValiant, "
                 "art by Poltergeist/Coffee Cup, Mid117, Wolfang62 and aethestode.")
# (LOOK, SHEET, size, sha256) at LENS_COMMIT.
PLAYER_FILES = (
    ('brendan_hgss', 'walk', 5586, 'd369353c8d9c1db2b002f38b6e61d08fe86b7ccba7940ff59e408cf9eb8a25f3'),
    ('brendan_hgss', 'bike', 3038, 'd84a9623c775e29c00201e593902ee10d2336efa932b715c4ca9e9ca4a564ae7'),
    ('brendan_hgss', 'acroBike', 5971, '6f1880d6613024735ff8f13e5cc25ee4c573a69fa62c476bd2b2745c8c1ef7f4'),
    ('brendan_hgss', 'surf', 2037, '8387854ef79ee3c83f81297c4b36176582740f34c927259680479eef95772912'),
    ('brendan_hgss', 'fieldMove', 2094, 'e4c4078190ec71133eb611acf8758faed4b1b516e7bc821fc6ce05996d47253e'),
    ('brendan_hgss', 'underwater', 1905, 'fffef50f1d09cad66be3a0fb2d85c877c5fca933c6c8d7128646d0f5cfd463e6'),
    ('brendan_hgss', 'fishing', 4365, '8ddeed75d44b8b48e042755de1715176f8d741d3c0545790cc672321f5534ef6'),
    ('brendan_hgss', 'watering', 1894, 'dbf0807b1524511c444142a554095edcded7346470c6385dcb9c7f75a6d129eb'),
    ('brendan_hgss', 'decorating', 760, '3b6b75e7cf83603617051d136492cc08d1f8716aad83c1e7aa7452d189612142'),
    ('may_hgss', 'walk', 6112, '4347f710a82fad02bece4060252cd4c38ef19fc69eba53d710351ed562a63e5c'),
    ('may_hgss', 'bike', 3214, '4dcd67f1dc945f68e91b7936f167b801c7a783631e2cb4dcd19903c02fa252f9'),
    ('may_hgss', 'acroBike', 6457, 'a46b44edfb9fc88265ab8c4388b36b7970b52efe09d069b18fa8a96c96541e5b'),
    ('may_hgss', 'surf', 1960, '18b97bc5d484e4f2e010014380b39ea70484c0d5712634dfa290b5088fc04729'),
    ('may_hgss', 'fieldMove', 1849, 'c0901cf85290fe926fdee5f33691643af1e70aed20671b87af45bac87a98a8bb'),
    ('may_hgss', 'underwater', 2018, 'b4d8309902c0b8a09431df279224790da3da03c4569cdc07e71b057b3d5ff391'),
    ('may_hgss', 'fishing', 4409, '97d8e1903ec1c8d52cee0d15421ac1836f8e323edb46cc4f0b7275c2b3a2ce70'),
    ('may_hgss', 'watering', 1822, '9a7e8e20f2e743361572422335929b34c320b536d6e3403751247e1d25bc7b22'),
    ('may_hgss', 'decorating', 637, '6eff233dee8fa9efa31f4be56a6c2663a532c363dc97b4b2953774c2a4bc9bec'),
    ('brendan_oras', 'walk', 3088, '182d05d151ad2205c0f3abff6cb302392f0b896e6c34eee3feff29f0d37d7a7c'),
    ('brendan_oras', 'bike', 2205, '8688d5317e76e0ada9035880e8c678ac8fba970a753b676ecaedf12c02409896'),
    ('brendan_oras', 'acroBike', 5806, 'e9e5e16b668762e9a946c03783548d6b8b549b8e20c8669b827e4ac7ae819234'),
    ('brendan_oras', 'surf', 2032, '559003a1a570a077b52c73f21ead5c00e0ae243c98e6ecceb786ddb8589592df'),
    ('brendan_oras', 'fieldMove', 1539, '640b6ffc74499c50ab2af5ce2c13d3b269a9b77cf2685e2a1ea86f2090b609cb'),
    ('brendan_oras', 'underwater', 1215, '221bf2b4604167b9b3d798d1be23f1f63c17abe675271a5e733c9d4c473807f0'),
    ('brendan_oras', 'fishing', 3141, '5d9dbceb30b45606ae22b884e86bdae2d18c30086b0e53e03050026d46fd0387'),
    ('brendan_oras', 'watering', 1631, 'd070bc9993386aabc1fe19de241d430ca25e813a05c7bb380c9d82a4355be36a'),
    ('brendan_oras', 'decorating', 541, '8432ff963b7c3b15322ed509bf269f2d4fafedeb8bc58a6f157596dfb8688d1f'),
    ('may_oras', 'walk', 3205, '8cb8e906a8ca9acaf3486ca5c7f38879dd2d2762506ca215b6c454b03ae7e312'),
    ('may_oras', 'bike', 2477, 'be2b9c0f995dfbc5dff6a2b58af3c599750033e3bcd4a35954434773c2cc9d12'),
    ('may_oras', 'acroBike', 6383, 'df64baf029ea4d2efd318f758da452c800927cf59856cdefcc035c5180672ba9'),
    ('may_oras', 'surf', 2316, '6d93505de5212587bed07c4e3e6f90f43d41c8600fc65acce28c8908ca2ceada'),
    ('may_oras', 'fieldMove', 1785, '7d96e2cfb2294ae01d31a772f8ee279308579c378ed0ab830caf5402171de8cf'),
    ('may_oras', 'underwater', 1288, '4efc6061e8474f4983c31ac004e7bd922dc58e6802c26c3a586ea8e971b80d0e'),
    ('may_oras', 'fishing', 3439, 'cdf99340348e2ac09bdf58f6104c73b62c32239add70ad233ff6ee78cddfb9be'),
    ('may_oras', 'watering', 1783, 'e8b3cb8e24c05f29dc1def0bc0537d6c898fbfe90592a7019cdce22739ad701b'),
    ('may_oras', 'decorating', 553, 'd433dae5d67074d2c71e41d2380781f331bb904215a5e6b1d0e2fe64e6a5a88f'),
    ('ash_hgss', 'walk', 4844, '4285a0de4901c2b5ae589ca70c7dad1a2de4c0ba9975fbf4a1800d760b9c1291'),
    ('ash_hgss', 'bike', 2650, 'b0682b395a7cf91f2a9a4ecbebef3b39e2de7988894f5976036101ce8fbb0dea'),
    ('ash_hgss', 'acroBike', 5051, '91f9f1daac0f8baed449a1c12bbe7eec340916d68f8dbd4e1077da1b4799f44a'),
    ('ash_hgss', 'surf', 1973, '6960ff349e736f581104fb81d59bc55b93fd98baa1705f8e14f29dc5f0ad6e0f'),
    ('ash_hgss', 'fieldMove', 1120, '6c74051ccfc863a8e8e7a61445461c52894590cb332145aa6a088090e794f65c'),
    ('ash_hgss', 'underwater', 1837, '6e221362ae34ed82057ae561b30d92b8f5e630cb318e66870a3bf53310a83d52'),
    ('ash_hgss', 'fishing', 4426, 'cea7677de4382786f0dbfeea029a423c208fae6f16077b13072339a69a2d90a1'),
    ('ash_hgss', 'watering', 1865, 'a97994cbdf76d2fbaf6b173ae65813772249b6778c91cf525dd495a5b8a959ad'),
    ('ash_hgss', 'decorating', 659, 'c9871ac1e30fdd3df702cf2b22440ebe873016db9809a80a57f080b32264aef1'),
    ('kris_hgss', 'walk', 5950, 'e8210438c60243e9504ead9de88bfe92b4c3f59c929f3798f9dc1812d35a54dc'),
    ('kris_hgss', 'bike', 3293, '91a5405a078de68b4548ab45a25159d392c9a09fca53384c1ff941aa94d1e0b0'),
    ('kris_hgss', 'acroBike', 6561, '268113fc08f0a3216d14b9007fd080b0d2b1a871505cab8778ef57c0bf5c2d3e'),
    ('kris_hgss', 'surf', 2002, '91b9bd469bf992135bd0ffcf33c449885ac5e7f19a9cdd508d208feef0075969'),
    ('kris_hgss', 'fieldMove', 1973, '5489adf28699174cee84b8d5128769c676b2088d0fef6152f513d4b88a74c7c3'),
    ('kris_hgss', 'underwater', 2008, 'a41e9e5bae2dbea186993d970df1e3bc97e095a9f5a9e132a96fa52e6a0f9825'),
    ('kris_hgss', 'fishing', 3768, '764b28ae382a0f5176c17d927dbf4c7f28434ad54a82df7071b4a9e5b7b52400'),
    ('kris_hgss', 'watering', 1875, 'd4cc90f73c797f51aaf90ce9f6d4b04c81c4b24dd0103acb42c924120cafe9dc'),
    ('kris_hgss', 'decorating', 711, '10ce06bcb0de3a1d2c15ad48696852976c484daf9ccabde32fb7f032b90bd649'),
    ('gold_hgss', 'walk', 4003, '99021ecb1012e2d5ed6ab26e9c0a4190ace0a0db3c96210341be88227c5625cf'),
    ('gold_hgss', 'bike', 2162, 'd569e32e88f07794c4307148090b68a7e1c5f4d92c9e19b916bfd4df9bea5051'),
    ('gold_hgss', 'acroBike', 4361, '8bf3ae3962be4117e830d7020906252d1feca83bb427e50c72e1118e45a223e9'),
    ('gold_hgss', 'surf', 1648, '8c7729098714ebc7382837a3153e2845a63c76b918ad5d08316202e58b7a7c36'),
    ('gold_hgss', 'fieldMove', 1741, 'c090af1bab5039561d6ef3ec59bef28b447b9a85fb741496cfaef3cd12a3ab2e'),
    ('gold_hgss', 'underwater', 1723, '388093935916e62112b4ad239bbf0f22c99c8589d86305af143d77e949e343b2'),
    ('gold_hgss', 'fishing', 3763, '2fb3a03d3076b28ffbdf519f14ba354dfb9150174ed376e944499c6c2f4be6ed'),
    ('gold_hgss', 'watering', 1578, 'f6dce8288b6c5c8bd6f56ca0a6f4bd4f417f5719237fab97f5f7e409d8ba4fcd'),
    ('gold_hgss', 'decorating', 586, 'e20fdbad78e7caa83b1cefee0a64a9b661f2c151f240db8741957804cac4d772'),
    ('silver_hgss', 'walk', 3567, '479639489971c9624141880c9633b46b49ae7bafcb8f5f62d09a3fc48efdaa1b'),
    ('silver_hgss', 'bike', 2123, '3acd353b7ba18a6cab804552cbd907f66aa5da5990e91de1c0a41405d43b047e'),
    ('silver_hgss', 'acroBike', 4309, '10de966fc581549cfae3a80cb7f44d56a1c3ef0dc4c4e64119f9715f22ed4500'),
    ('silver_hgss', 'surf', 1585, '4e25338ea6b6d5caa1802f76361f6add2b5ab57de5fd09d3b4b475245053fd13'),
    ('silver_hgss', 'fieldMove', 1643, 'c9623bb3f9fb618ee547edfb1e5283ffef78e8f018e645abc6c71c76f6b3eb3f'),
    ('silver_hgss', 'underwater', 1608, '4a9c7fe8adde67162b295147a92043ca51d6f74619c09fa1c06c8ac878494137'),
    ('silver_hgss', 'fishing', 3319, '6437fa9242dec770f6f01473d6910e29c2f7facaf2f4c1d1ed7b26d785296f24'),
    ('silver_hgss', 'watering', 1492, '6e9cb4c4333c47bb6b52a4bc54ca9e994fc5ea31a0207df22d46cbe481802267'),
    ('silver_hgss', 'decorating', 512, '1198776fcd55e3782ffede76d53ea8f5ff291b2ab66aa1db9826ad302f58bc33'),
    ('green_hgss', 'walk', 3372, '87a06d4c4c2580fcaf407811d6dbb95e1a114411c386aecaec1e12ee7f415aba'),
    ('green_hgss', 'bike', 1986, '39539a15291cf8e557fad2e2217bad20bcc026d07db743aeebbc0c78d5b213b9'),
    ('green_hgss', 'acroBike', 3953, '0e2a8551e577a091a33c7f250dc7de0ab0385fde745a4c89cdc9c9d59499d982'),
    ('green_hgss', 'surf', 1444, 'fc826ed8bd76eb44354c27bf9d8b6a97f220ee7c6fabeb76883371cf8b4f04da'),
    ('green_hgss', 'fieldMove', 754, 'e078c523958020368924c3edc12b1d514bd1ee3a515e7f8cd410b5d874939d5b'),
    ('green_hgss', 'underwater', 1422, '0a00e9190da6171426443e5ab58b0c4796567437a5f6785f6d3d42cae2ef9419'),
    ('green_hgss', 'fishing', 3329, '328ea9352d000a22b72f11798aa11108ce71510a97a66cf46185a1113b361a19'),
    ('green_hgss', 'watering', 1457, '3a1d724fbe92b392ff570ed883528e2a63b55a13552cde432689bebfa0a27477'),
    ('green_hgss', 'decorating', 500, 'd41af40cb3316c40e1afc34dc44fbdc405553592fc8d3114abfc5093f019128a'),
    ('brendan_oras', 'back', 4792, '26ba2ffdc08bde4c6d0a7378853ed2d2f7c3beb8702f4cb1eb48b1110ba892b2'),
    ('may_oras', 'back', 4339, 'ac2d691775d104c2481e10cab4e79d5d46700f5f103bfc07ad89bae42c7a7d68'),
    ('ash_hgss', 'back', 6099, '86ad130775d8f48ae226a0017055f6496c95aea8baf1c335a4aff9e7830f0872'),
    ('kris_hgss', 'back', 6240, 'fc66c63e749cd7dea6234441e3b295ff487a42b5fa2f8d20c221a0969ac0472b'),
    ('silver_hgss', 'back', 5051, '746832789f74cf4c8f1b44be068414bd10dd4222e43afcb77699beacda8b1993'),
)

# The people, by the set that draws them: (set, NAME, size, sha256). Filled by
# --list from a clone at COMMIT (the names Emerald has).
FILES: tuple = (
    ('community', 'ANABEL', 1501, '1cfa98dbe5480ba51fcab31a928bc88943a98fca460bae8ca171438ebbcb2323'),
    ('community', 'AQUA_MEMBER_F', 2714, 'fdd2cf9538a31eae4f6b12313267d64bbf5cdb951efe73a3cdf593aade045f0d'),
    ('community', 'AQUA_MEMBER_M', 2638, '8bbd85bd901837c064b6e0bb9f30ba02a6b2f8a983d165e3ac1fa240718441a8'),
    ('zaffre', 'ARCHIE', 2165, '9949b19756604ed36b28746206db6fd1ae4791893e89a8b00f6069feff9cbb87'),
    ('diego', 'ARTIST', 2670, '13f87d5feebcec8188b69618f08ee1e60ca9c809453bdf13aaff12fa20ed0669'),
    ('community', 'BARD', 2052, 'd3a4c2037c2d5557c003b1244aee0bc1bbaa382842f1c0a04cebb782b372ee6f'),
    ('diego', 'BEAUTY', 2418, 'ecd85827d671874879df235920e102f258674f26c7a8bdec7d637083a0f9c6a8'),
    ('diego', 'BLACK_BELT', 2614, '8c2364f1a87b3c655f13a45ff6626fb8e7292b455046805c9fd34d4f9c279410'),
    ('diego', 'BOY_1', 2023, 'd3edad2404a01f741e825170bfc69bf0cc2fbe27e05573cd7b72e40b02088755'),
    ('diego', 'BOY_2', 1969, '1e6bf404243554e44e76db709eca28528ab4981d5dfc07b8804f21d309f40da9'),
    ('diego', 'BOY_3', 1722, '3251a0174d4f0c10e1c362b8432c463c9cda48c8e3b356596d6d91e007340056'),
    ('community', 'BRANDON', 1471, '80ba0015a48e6b99c36fdf4e8c0ad18b987a1556ba95651e5c4df104d239dc17'),
    ('zaffre', 'BRAWLY', 2640, 'fc900f4f234817c8e4af54698cc72ae355930eb90846fbe8ce3df7e6bac41c8f'),
    ('taiga', 'BUG_CATCHER', 2283, '94c1d2ba16317c06d5f993f20ef284d2fd5adc8604794622b1434b3e5f6a7e70'),
    ('community', 'CAMERAMAN', 2113, 'f426abe70ffdc21ae1516a9fd945878d2f7758b307704f2f92ef8622a4b9d1f4'),
    ('taiga', 'CAMPER', 2293, 'e6f38fff59cd33d6fabc2de0ea176d05d7e8395aa790bd755116a628652885e2'),
    ('diego', 'CONTEST_JUDGE', 2045, '0edca25760ee955358c53981b8bbde9344f1c33cf99d162a45a6760c2df359cd'),
    ('diego', 'COOK', 2334, '1074cf14d8a9fa013893948cceec162737686d8d00cdf7dba30fbee40e655f6b'),
    ('diego', 'CYCLING_TRIATHLETE_F', 2748, 'db78c57555d6771987f70c0a9d5b28b9ddd8723f410edaf22fb5c60fb729eabd'),
    ('diego', 'CYCLING_TRIATHLETE_M', 2494, 'ed1d45c8e5f3582e12a73efbed38f9dadaf1e354de7a22e44b90b2671228956b'),
    ('community', 'DEVON_EMPLOYEE', 2342, '603ba98c30bbe5fb1985d2eab7357601730ac533f8c62b12c5df51e00c14d2b0'),
    ('community', 'DRAKE', 2695, '159577257a04e81330fc2445e5e616e8ee04dac83b9720c8172fe8f44bba72f2'),
    ('community', 'EXPERT_F', 2104, '084d9793462e0d9c13e51514b4ab68e044cb1c6c263cba1d80b864ca56327538'),
    ('community', 'EXPERT_M', 2187, 'd102ad266117cb5e911cb429eb5dc92c774eaade54bc0cf29176b8b8903b2e73'),
    ('community', 'FAT_MAN', 2425, '63b6f70c675550bbecfa20706a5b92cce8789349a04bd154da8fb0abd429bd9e'),
    ('diego', 'FISHERMAN', 2496, 'ede4622165f704ec0484316fb7d06f51f590581e2483cb9a4d95dfcd0f983572'),
    ('zaffre', 'FLANNERY', 3066, '6491e41370ec81dfb9e086eb6a5cbda15413b8e607dfcb4d612eb15ed918eca8'),
    ('community', 'GAMEBOY_KID', 2389, '72f7333bfeded9ea8c41c6a64938d752a9cd6fc2c53919172b0dcdff09bc069b'),
    ('diego', 'GENTLEMAN', 2303, '178d0882c2c1661334fda5ee74a99c3762758e847ae1dfe9a9b3026eeb1c7dcb'),
    ('community', 'GIRL_1', 2041, 'f069dfb2aa18a69abe95fe6816a7d3170095286fc2fa2ef5f30a512da0eaa2f2'),
    ('community', 'GIRL_2', 1778, '601b4ad3d792c402fa58fef9c2c193083fb4f1a749b4fb9c46dc2b608a2ea5fc'),
    ('diego', 'GIRL_3', 2207, '7636a664691600e5c80058fb891ac8feafadbf4a41a908136f37a3a3711961e2'),
    ('community', 'GLACIA', 1982, '4802af956c42764abc106fb1f566ba1195225c1be8fc363e73a9f2c9ca020f00'),
    ('community', 'GRETA', 1485, '38353611e4e0c9179629c63cc23d7caf6dd0958a92901a1f1f4361d892920900'),
    ('community', 'HEX_MANIAC', 2930, '1368c1c3a234ca14b0587c60fb0e4f64c9b6409ff53f4f9debecd7f3eafd0926'),
    ('diego', 'HIKER', 2352, 'c44108dc43a4c95115e6cec2fa8ad3f608361acb89c6e643c8e8493afe87eba7'),
    ('diego', 'HOT_SPRINGS_OLD_WOMAN', 1790, '4c8925180f0a0f493da978c4978e0ba7eac7f0455da48cd7ea9b9df1616849f8'),
    ('zaffre', 'JUAN', 2597, 'df43efd67bd984a067066f1d28109f76083569bb382962a2979573108b96eb36'),
    ('diego', 'LASS', 2636, '09d0189611a651e9b6d34373fd8b5458008ba39bfa7b5870084e7167bc46acdc'),
    ('community', 'LINK_RECEPTIONIST', 1869, '5e8e98d12b8f1baa65a92d6aa127a0c9872ab35f2435bde9d5810b820f3be672'),
    ('community', 'LITTLE_BOY', 1727, '774ba68a4057f5fa7041b9bb8f0b7df99d41511cf390580b1d8900c2ec10e2b2'),
    ('diego', 'LITTLE_BOY_3', 1643, '716ed945ecbe8ae886cbfb47d642a37fe3bec4bf5f0c27313c01e4ae4defa2bb'),
    ('community', 'LITTLE_GIRL', 2000, '805b7475a29dcc963981bc55a61199dd2bbc23d19db8c1ac7ed0297f38f2140a'),
    ('zaffre', 'LIZA', 2272, '9e292cb7c0d520d5399ea088bb06cf81a73011e5162918c432d9f44d69119411'),
    ('community', 'LUCY', 3227, '5761a4f0ec239912c69a5441763b0b962ca267ef19780678a0586cebaed5af4f'),
    ('community', 'MAGMA_MEMBER_F', 2103, '80fcacc7a14f20f5d41d8563224b91ad4d2f6f73ae644e2708e9302520bc127b'),
    ('community', 'MAGMA_MEMBER_M', 1992, '19dc85b268889633427d064784318e6efeefd9c6457c816073dac138051613ae'),
    ('taiga', 'MANIAC', 2438, 'c471f1f38a19ccdba03ba69c0c9a478d6c0309203719e3a9978600a0253a073a'),
    ('community', 'MAN_1', 2277, '1119ba41db634865c67db1740a54be9870be38a80320b37d69c51f6e30aeca19'),
    ('community', 'MAN_2', 2104, '37bbdf6ac653feb3e34671e830a8e3a154caec48a97c7ccc0b11a08f993dcb6c'),
    ('diego', 'MAN_3', 2034, '9e23887e621740d9ebe15bc84af51e422521393ad97643239ea5dca65bf5ea97'),
    ('diego', 'MAN_4', 2279, 'cb41b44d832d3704f254f071fa3127fccda6f90ba9ae9bbae0b08217cceae892'),
    ('zaffre', 'MAN_5', 2367, '053e92ee0006ed9a2fad0079b6cb0a736b61351aac287d035716ac939572fc1f'),
    ('diego', 'MART_EMPLOYEE', 1909, 'c5ab73f3f25be514ab2ed14d8df0d43c8e8d30962eb4afa29d69d6aa32e14fbf'),
    ('zaffre', 'MAXIE', 2428, '2592cef32173735836b5d4ffa1e6d8ff539b6f3eb3ef8a55a9c157852e411e6f'),
    ('community', 'MOM', 2285, 'cbbec0e8a862e1c6dc035990cdfb74234a7fe385e630df6a3a719b07d701da34'),
    ('taiga', 'MYSTERY_GIFT_MAN', 2779, '018bbd0edec470590be71f05335f108c0be8f2b119343b5cead175564986629d'),
    ('taiga', 'NINJA_BOY', 2041, '1ea45fcfd5623db6daf299c9abb7c8385b1c9ed58656eb08302ccd20a1c59126'),
    ('community', 'NOLAND', 1404, '863c245d70d1df751e8e234e041077e44a2b70476edd8b0cb38ea82d8b3c1836'),
    ('zaffre', 'NORMAN', 1980, '371ecd358d2cf1b43f2e0804ae59817e9a129d12b1510a032f4fe41b6902f62a'),
    ('diego', 'NURSE', 2472, '3c5133e123abb20355953ec479e494256dfa500cc7bbc126d9e332d5deb3d1cf'),
    ('community', 'OLD_MAN', 1787, 'f0a29cc6cca67572269358847ca58edbfbd8eb4f1c98e5309c611e0c69d64c72'),
    ('community', 'OLD_WOMAN', 1733, '75323302fd25f23550d490e22bcca703432a665573b071327026f9fe408c262d'),
    ('community', 'PHOEBE', 2969, '9d932af72540fa82be61bf8c137cf528fe038f14a19c1eb38653a7d510186184'),
    ('community', 'PICNICKER', 2600, '288d05f1dcd306679c42fd5e013fc0af8ba2c58a46007e0ec5689906ddd5e366'),
    ('diego', 'POKEFAN_F', 2178, '1ca0babba16726fdf4e844ca4ee74fff5892c4ab351dcd618c61c64675bb8051'),
    ('diego', 'POKEFAN_M', 2221, '3846638fd588b4e2c141da37a137e0eeb2e340b98f7c055080c899604bcd0906'),
    ('community', 'PROF_BIRCH', 2763, 'c24af6a00e90c18805336e01ee1171bdec312641c654347ad07065d1a4d0bf7f'),
    ('diego', 'PSYCHIC_M', 2396, '9a49ec5b14d004ede29402f9da29b1823e053ebf86c0dae482441fbc8d417cd8'),
    ('community', 'REPORTER_F', 1821, 'abe96a75188c62dc75d7f6d76c3013de15ec3ff47f35ff551c3f1ff5b6757d3c'),
    ('community', 'REPORTER_M', 2719, '5752c40e1a74c8ef169530cfaa9792f66b41f8288a4241f513b549d2cf534595'),
    ('diego', 'RICH_BOY', 2204, '39ff035646ab51eddb515fdd96a18d62324412d3e3ab21b589da0f4530e3e62c'),
    ('diego', 'ROOFTOP_SALE_WOMAN', 2324, 'c24763a8ed7fb7edfc6c050afbe3ced45b766e23e776554d497c5276b64e6c37'),
    ('zaffre', 'ROXANNE', 2437, 'be084aeed7c91e9d38c98661ca97ee012a6affe0544cdd72ffa9bdd80477c379'),
    ('community', 'RUNNING_TRIATHLETE_F', 3527, '39edcf94b1a5b7437eb6637c2f63dff0032e77d84804ca42a222dc9002a81952'),
    ('community', 'RUNNING_TRIATHLETE_M', 2742, 'd496da377cfef38a39738eba231245469e341b6ce5481d15b28cb090616e0b29'),
    ('taiga', 'SAILOR', 3196, '4ec7008e65284632a9ee3553c574ceaa3216aa14150d4d6b7f57da313aaf8edc'),
    ('diego', 'SCHOOL_KID_M', 2168, '38860420bb4eb63b0d019e5fde12a5d9ae6bbe770c7806c6850b3105a6619fa5'),
    ('diego', 'SCIENTIST_1', 2283, '123eb88028821953a7b3306ebd53bacf62b03032a908b65354d558d423ac0710'),
    ('community', 'SCIENTIST_2', 2298, '33268addfb2ec89619641140335a06929fe2f3763408cac56995f14641341cf1'),
    ('community', 'SCOTT', 3122, 'e346ed9bd6e693456d284bb8ab0e50ef35d25a0274b5ef5d5daa51caa8f584d6'),
    ('community', 'SIDNEY', 2215, '2c7e38b8508e34e8d4e5646b72d8e6d127d40c51fe892743e38fbc4c97357726'),
    ('community', 'SPENSER', 1622, 'a6093be047797bcbc4b51fb1c828f27922fb1020914458ead4250683b07f31e1'),
    ('community', 'STEVEN', 2400, 'd77c7ffca18d8e91209de80900de6f3aa09b602f672a063d722a1139e0e47e1e'),
    ('community', 'SWIMMER_F', 2049, 'fafe4566ae710d1576bbedcb15612139d016c5411dba9e8e4bd373107927a7c7'),
    ('diego', 'SWIMMER_M', 1749, '534b058e14715db9a73b11bb4a54c06d8274623d7f13e116e43c53d827891e0e'),
    ('zaffre', 'TATE', 2233, 'c506d23f4d924ae5055235e5d62e4367a7856e17d767946646ea7602a0ee7c05'),
    ('community', 'TEALA', 1774, '245a577490d7183544bd787d458285e151994408df095ec13a51f531019b4e9f'),
    ('taiga', 'TUBER_F', 1433, '178aecefde05c655f91c46eada1d681ab081a00c6ba4bfb838d98ee90cb4149f'),
    ('taiga', 'TUBER_M', 1306, '8fb4266b3376d314283dfb46c32b6eb397a611134d3c778d90f4c984e0d86e92'),
    ('community', 'TUBER_M_SWIMMING', 1047, '31532db27232ee5ef3cdf3ef7c7f3249241b42cb207d58b8f04bf7ad72b682cf'),
    ('community', 'TUCKER', 1876, '31c39f09d9c48a6a91a957c29031a336a506b3475a8dce50057e12f8e475e99d'),
    ('diego', 'TWIN', 1877, '39a7cbd78070b4386de66713a9a0cbf5b9a74aa92728de29754a59a7b11f4805'),
    ('community', 'UNION_ROOM_NURSE', 1952, '9bcf25073656b64d54ef4d0d3241e9e3a76f9106376805594d26cfd5127d0d76'),
    ('zaffre', 'WALLACE', 2256, '420d6c2493707cb987ad943ac4ff74d8198e79296e8ef9c4e6bb6ee330296595'),
    ('community', 'WALLY', 2701, '8f84ae28c0ffb425678e9e3f32fd3556ba55d4377f7d12404f76411e08df8c97'),
    ('zaffre', 'WATTSON', 2605, 'dba8747da4f0a89e983b2817a1aeca31deab1f32ce7d939555f2708c70db4874'),
    ('zaffre', 'WINONA', 2760, '9dc07bc80da58ecc442431f775d50182cf8c47dabe392dc7682046ab39660549'),
    ('community', 'WOMAN_1', 2493, '8d18c0498a72f4a985af7b65a25295d72558a3fc7749dffd15b42d7fe4cf260c'),
    ('diego', 'WOMAN_2', 2277, '5ea5df9c08e2e808493e58f6c690c6243927d3eac72a0593b47c3bf416bdbb74'),
    ('diego', 'WOMAN_3', 1961, '7482565a20386f873f035cae6a1cf8619a490c1851469fb25073c1f8e460d29a'),
    ('community', 'WOMAN_4', 2512, '1053c64080ab1a82a08d691d07b899f4ac088981c263f37003bc227b1dd1f800'),
    ('diego', 'WOMAN_5', 2924, 'f4a558111df0bf78c8a98b01fddbd5013ebec0bacb5aeec163f53d697cb819b7'),
    ('diego', 'YOUNGSTER', 2147, '6b7116a5a551953556ff6e7acbde04cccc905830f25b6dcf736a344c86505d4e'),
)


def _sets_path(set_name: str, name: str) -> str:
    return f"assets/npcs/{set_name}/gen3/rse/{name}.png"


def list_files(clone: str) -> list:
    """The look of each person, the first set that has one, from a clone."""
    chosen: dict = {}
    for set_name in SETS:
        folder = os.path.join(clone, "assets", "npcs", set_name, "gen3", "rse")
        if not os.path.isdir(folder):
            continue
        for entry in sorted(os.listdir(folder)):
            if not entry.endswith(".png"):
                continue
            name = entry[:-4]
            if name in chosen:
                continue
            with open(os.path.join(folder, entry), "rb") as f:
                data = f.read()
            chosen[name] = (set_name, name, len(data), hashlib.sha256(data).hexdigest())
    return [chosen[n] for n in sorted(chosen)]


def fetch(set_name: str, name: str, size: int, digest: str, clone: str | None, path: str | None = None,
          repo: str = REPO, commit: str = COMMIT) -> bytes:
    path = path or _sets_path(set_name, name)
    if clone:
        with open(os.path.join(clone, path), "rb") as f:
            data = f.read()
    else:
        with urllib.request.urlopen(URL.format(repo=repo, commit=commit, path=path), timeout=60) as r:
            data = r.read()
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"{path}: not the pinned file")
    return data


def bgr555(rgb) -> int:
    r, g, b = rgb
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def convert(png: bytes, frames: int | None = FRAMES, size: int = SIZE) -> tuple:
    """(the .bin, the RGBA sheet as shown): at most 15 colours and transparency."""
    from PIL import Image

    sheet = Image.open(io.BytesIO(png)).convert("RGBA")
    if sheet.width == size // 2 and frames is None:
        wide = Image.new("RGBA", (size, sheet.height))
        wide.paste(sheet, (size // 4, 0))
        sheet = wide
    if frames is None and sheet.width == size and sheet.height % size == 0 and 0 < sheet.height // size < 64:
        frames = sheet.height // size
    if sheet.size != (size, size * (frames or 0)):
        raise ValueError(f"sheet of {sheet.size}, not {size}x{size * (frames or 0)}")
    pixels = list(sheet.getdata())
    opaque = [p[:3] for p in pixels if p[3] >= 128]
    colours = sorted(set(opaque))
    if len(colours) > 15:
        # Down to 15 by median cut over the opaque pixels alone.
        strip = Image.new("RGB", (len(opaque), 1))
        strip.putdata(opaque)
        reduced = strip.quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        flat = reduced.getpalette()[:15 * 3]
        colours = [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
        colours = sorted(set(colours))

    def nearest(c):
        return min(range(len(colours)), key=lambda i: sum((a - b) ** 2 for a, b in zip(colours[i], c)))

    cache: dict = {}
    index = []
    for p in pixels:
        if p[3] < 128:
            index.append(0)
            continue
        k = p[:3]
        if k not in cache:
            cache[k] = nearest(k) + 1
        index.append(cache[k])
    palette = [0] + [bgr555(c) for c in colours] + [0] * (15 - len(colours))
    out = bytearray(MAGIC + bytes((frames, size, size, 0)) + struct.pack("<16H", *palette))
    for frame in range(frames):
        for ty in range(size // 8):
            for tx in range(size // 8):
                for y in range(8):
                    row = (frame * size + ty * 8 + y) * size + tx * 8
                    for x in range(0, 8, 2):
                        out.append(index[row + x] | (index[row + x + 1] << 4))
    shown = Image.new("RGBA", sheet.size)
    shown.putdata([(0, 0, 0, 0) if i == 0 else colours[i - 1] + (255,) for i in index])
    return bytes(out), shown


def pack_paths() -> list[str]:
    """Every path the section can hold."""
    return ([PREFIX + name + ".bin" for _, name, _, _ in FILES] + [PREFIX + "CREDITS.txt"]
            + [f"{PLAYER_PREFIX}{look}/{sheet}.bin" for look, sheet, _, _ in PLAYER_FILES]
            + [PLAYER_PREFIX + "CREDITS.txt"])


def build_section(clone: str | None = None, lens_clone: str | None = None, progress=None,
                  shown: list | None = None) -> tuple[dict[str, bytes], dict]:
    """The pack's npcs/ and players/ files, and a summary {people, players,
    missing}. A file that cannot be had or is not the pinned one is left out
    (that person keeps the game's look, that look keeps the game's sheet);
    the build goes on."""
    files: dict[str, bytes] = {}
    missing = []
    total = len(FILES) + len(PLAYER_FILES)
    done = 0
    for set_name, name, size, digest in FILES:
        try:
            data, sheet = convert(fetch(set_name, name, size, digest, clone))
            files[PREFIX + name + ".bin"] = data
            if shown is not None:
                shown.append((name, sheet))
        except Exception as error:      # noqa: BLE001 - optional art, never fatal
            missing.append(f"{_sets_path(set_name, name)}: {error}")
        done += 1
        if progress and done % 20 == 0:
            progress(done / total)
    files[PREFIX + "CREDITS.txt"] = (CREDIT + "\n" + f"https://github.com/{REPO} at {COMMIT}\n").encode("utf-8")
    for look, sheet_name, size, digest in PLAYER_FILES:
        path = f"assets/sprites/{look}/gen3/rse/{sheet_name}.png"
        try:
            data, sheet = convert(fetch(look, sheet_name, size, digest, lens_clone, path, LENS_REPO, LENS_COMMIT),
                                  None, 64 if sheet_name == "back" else SIZE)
            files[f"{PLAYER_PREFIX}{look}/{sheet_name}.bin"] = data
            if shown is not None and sheet_name == "walk":
                shown.append((look, sheet))
        except Exception as error:      # noqa: BLE001
            missing.append(f"{path}: {error}")
        done += 1
        if progress and done % 20 == 0:
            progress(done / total)
    files[PLAYER_PREFIX + "CREDITS.txt"] = (PLAYER_CREDIT + "\n"
                                            + f"https://github.com/{LENS_REPO} at {LENS_COMMIT}\n").encode("utf-8")
    people = sum(1 for p in files if p.startswith(PREFIX) and p.endswith(".bin"))
    players = sum(1 for p in files if p.startswith(PLAYER_PREFIX) and p.endswith(".bin"))
    return files, {"people": people, "players": players, "missing": missing}


def build(out_dir: str, clone: str | None, preview: str | None, lens_clone: str | None = None) -> int:
    shown: list = []
    files, info = build_section(clone, lens_clone, shown=shown)
    for path, data in files.items():
        full = os.path.join(out_dir, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    for line in info["missing"]:
        print("npc_looks: left out " + line, file=sys.stderr)
    if preview:
        from PIL import Image

        cols = 12
        rows = (len(shown) + cols - 1) // cols
        page = Image.new("RGBA", (cols * SIZE * 3, rows * SIZE), (200, 200, 200, 255))
        for i, (_, sheet) in enumerate(shown):
            for f, frame in enumerate((0, 1, 2)):
                part = sheet.crop((0, frame * SIZE, SIZE, (frame + 1) * SIZE))
                page.alpha_composite(part, ((i % cols) * SIZE * 3 + f * SIZE, (i // cols) * SIZE))
        page = page.resize((page.width * 2, page.height * 2), Image.NEAREST)
        page.save(preview)
    return info["people"] + info["players"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", help="the RomFS or pack folder to write npcs/ into")
    parser.add_argument("--sources", help="a clone of Facets at the pinned commit, instead of fetching")
    parser.add_argument("--lens-sources", help="a clone of Emerald Lens at the pinned commit, instead of fetching")
    parser.add_argument("--preview", help="a PNG of every look's first three frames")
    parser.add_argument("--list", action="store_true", help="print FILES from --sources")
    args = parser.parse_args(argv)
    if args.list:
        for entry in list_files(args.sources):
            print(f"    {entry!r},")
        return 0
    if not args.out:
        parser.error("--out is needed")
    try:
        count = build(args.out, args.sources, args.preview, lens_clone=args.lens_sources)
    except Exception as error:      # an optional section: the game keeps its own people
        print(f"npc_looks: not built ({error})", file=sys.stderr)
        return 1
    print(f"npc_looks: {count} sheets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
