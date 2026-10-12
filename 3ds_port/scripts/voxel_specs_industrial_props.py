"""Distinct industrial furniture from its own source art.

Canonical pieces match every equal drawing in their tilesets, so stools,
computer desks and cabinet banks are modelled once even across both floors.
Only furniture pixels are raised; floor/wall pixels stay in map patches.
"""
from functools import partial
from PIL import Image
from voxel_building import LayoutArt, Lifted, Mesh, Relief, Tile


def furniture(name, layout, rect, height, ground, background=(), trim=None):
    src=LayoutArt(layout)
    x,y,w,h=rect
    art=src.building_art(x,y,w,h,src.ground_tiles(ground))
    colors=set()
    for metatile in background:
        colors.update(src.cell_image(metatile).getdata())
    pixels=art.load()
    for v in range(h*16):
        for u in range(w*16):
            if pixels[u,v] in colors or (trim and not trim(u,v)):
                pixels[u,v]=(0,0,0,0)
    return {'name':name,'layout':layout,'rect':rect,'ground':list(ground),
            'art':art,'relief':{'height':height},
            'exact':[(0,0,w*16,h*16)]}


SPECS=[
    furniture('space_center_computer_desk','LAYOUT_MOSSDEEP_CITY_SPACE_CENTER_1F',
              (10,4,2,2),8,(0x2f0,)),
    furniture('space_center_schematic_table','LAYOUT_MOSSDEEP_CITY_SPACE_CENTER_1F',
              (2,5,2,2),8,(0x2f0,)),
    furniture('space_center_empty_desk','LAYOUT_MOSSDEEP_CITY_SPACE_CENTER_2F',
              (8,6,2,2),8,(0x2f0,)),
    furniture('space_center_wall_console','LAYOUT_MOSSDEEP_CITY_SPACE_CENTER_1F',
              (1,1,2,1),12,(0x2f0,),trim=lambda u,v:v<16),
    furniture('new_mauville_generator','LAYOUT_NEW_MAUVILLE_INSIDE',
              (32,1,4,4),24,(0x210,0x21f,0x227,0x275,0x27d)),
    furniture('new_mauville_equipment_rack','LAYOUT_NEW_MAUVILLE_INSIDE',
              (27,0,2,3),24,(0x210,0x21f,0x227,0x275)),
    furniture('new_mauville_green_machine','LAYOUT_NEW_MAUVILLE_INSIDE',
              (29,1,2,3),24,(0x210,0x21f,0x227,0x275,0x27d)),
    furniture('new_mauville_power_cabinet','LAYOUT_NEW_MAUVILLE_INSIDE',
              (37,1,1,2),16,(0x210,0x21f,0x227,0x275,0x27d)),
]


class OffsetTexture:
    def __init__(self, part, offset): self.part,self.offset=part,offset
    def emit(self,mesh):
        local=Mesh();self.part.emit(local)
        for tri,shade,tag in local.tris:
            mesh.tris.append((tuple((x,y,z,u,v+self.offset) for x,y,z,u,v in tri),shade,tag))


_generator=next(s for s in SPECS if s['name']=='new_mauville_generator')
_generator_drawing=_generator['art']
_generator_body=Image.new('RGBA',(64,64))
_gold={(230,156,65),(180,115,65),(238,189,139),(139,106,65),(172,255,57)}
for _u in range(4,60):
    _rows=[v for v in range(64) if _generator_drawing.getpixel((_u,v))[:3] in _gold]
    if not _rows:continue
    for _v in range(max(0,min(_rows)-1),min(64,max(_rows)+2)):
        _generator_body.putpixel((_u,_v),_generator_drawing.getpixel((_u,_v)))


def computer_desk_parts(desk,laptop):
    return [OffsetTexture(Relief('desk',desk,8,Tile(20,24,28,32)),32),
            OffsetTexture(Lifted(Relief('laptop',laptop,8,Tile(4,6,12,14)),8),64)]


_computer=next(s for s in SPECS if s['name']=='space_center_computer_desk')
_computer_drawing=_computer['art']
_laptop=Image.new('RGBA',(32,32))
_desk=_computer_drawing.copy()
for _v in range(14):
    for _u in range(2,16):
        _laptop.putpixel((_u,_v),_computer_drawing.getpixel((_u,_v)))
        _desk.putpixel((_u,_v),(0,0,0,0))
_computer_texture=Image.new('RGBA',(32,96))
_computer_texture.paste(_computer_drawing,(0,0))
_computer_texture.paste(_desk,(0,32))
_computer_texture.paste(_laptop,(0,64))
_computer['art']=_computer_texture
_computer['drawing']=_computer_drawing
_computer.pop('relief')
_computer['parts']=partial(computer_desk_parts,_desk,_laptop)

# The orange machine has a separate round exhaust cap above its counter.
# Its native circular outline is a real eight-pixel raised solid.
_generator_cap=Image.new('RGBA',(64,64))
_cap_top=[10,8,6,5,4,4,4,4,4,4,4,4,4,4,5,6,8,10]
_cap_bottom=[20,21,22,23,24,24,24,24,24,24,24,24,24,24,23,22,21,20]
for _i,(_top,_bottom) in enumerate(zip(_cap_top,_cap_bottom)):
    _u=23+_i
    for _v in range(_top,_bottom):
        _generator_cap.putpixel((_u,_v),_generator_drawing.getpixel((_u,_v)))
        _generator_body.putpixel((_u,_v),_generator_drawing.getpixel((20,20)))

# The plinth supports the complete machine, including its unseen rear.
# Removing the body from its top left a hollow white frame from behind.
_generator_platform=_generator_drawing.copy()
for _v in range(64):
    for _u in range(64):
        if _generator_body.getpixel((_u,_v))[3]:
            _generator_platform.putpixel((_u,_v),_generator_drawing.getpixel((4,60)))

for _v in range(8):
    for _u in range(64):
        _generator_platform.putpixel((_u,_v),(0,0,0,0))
        if not _generator_body.getpixel((_u,_v))[3]:
            _generator_drawing.putpixel((_u,_v),(0,0,0,0))

def generator_parts(platform,body,cap):
    return [OffsetTexture(Relief('generator_plinth',platform,4,
                                Tile(4,60,12,64),solid=True),64),
            OffsetTexture(Lifted(Relief('generator_body',body,24,
                                       Tile(24,32,32,56),solid=True),4),128),
            OffsetTexture(Lifted(Relief('round_exhaust_cap',cap,8,
                                       Tile(28,18,32,22),solid=True),28),192)]

_generator_texture=Image.new('RGBA',(64,256))
for _offset,_drawing in [(0,_generator_drawing),(64,_generator_platform),
                         (128,_generator_body),(192,_generator_cap)]:
    _generator_texture.paste(_drawing,(0,_offset))
_generator['art']=_generator_texture
_generator['parts']=partial(generator_parts,_generator_platform,_generator_body,_generator_cap)
_generator.pop('relief',None)
_generator['drawing']=_generator_drawing

_generator['preserve_ground']=True
_generator['own']={(u,v) for u in range(64) for v in range(64)
                   if _generator_drawing.getpixel((u,v))[3]>=128}
