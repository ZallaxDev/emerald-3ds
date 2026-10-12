"""Small solid props repeated across maps, isolated from their ground art."""
SPECS = [
    {
        'name': 'mt_pyre_gravestone',
        'layout': 'LAYOUT_MT_PYRE_1F',
        'rect': (13, 3, 1, 1),
        'ground': [0x2E6],
        'relief': {'height': 10, 'hull': 12, 'upper': True},
    },
]

# Display furniture is matched by its complete metatile rectangle. Counters,
# room walls and floor shadows are intentionally outside these footprints.
SPECS += [
    dict(name='museum_column', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_1F',
         rect=(2,3,1,2), ground=[0x201],
         relief=dict(height=24,hull=24,upper=True)),
    dict(name='museum_display_case', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_1F',
         rect=(15,3,1,2), ground=[0x201],
         relief=dict(height=16,hull=16,upper=True)),
    dict(name='museum_round_exhibit', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_2F',
         rect=(2,3,4,2), ground=[0x201],
         relief=dict(height=8,hull=10,upper=False)),
    dict(name='museum_ship_exhibit', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_2F',
         rect=(2,6,4,2), ground=[0x201],
         relief=dict(height=8,hull=10,upper=False)),
    dict(name='museum_computer_exhibit', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_2F',
         rect=(12,3,2,2), ground=[0x201],
         relief=dict(height=8,hull=10,upper=False)),
    dict(name='museum_boat_exhibit', layout='LAYOUT_SLATEPORT_CITY_OCEANIC_MUSEUM_2F',
         rect=(14,6,2,2), ground=[0x201],
         relief=dict(height=8,hull=10,upper=False)),
]
