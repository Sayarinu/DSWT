# Wonder Trade's X-menu icon: a 40x32 sprite laid out like the menu's own
# icons (wt_menu_icon_cell.json) and coloured with the menu's icon palette.
wonder_trade_menu_icon_ncgr = ncgr_cell_gen.process(
    files('wt_menu_icon.png'),
    extra_args : CLOBBER_SIZE_VERSION101_NCGR_FLAGS + ['-mappingtype', '32'],
)

wonder_trade_menu_icon_narc = custom_target('menu_icon.narc',
    command : [
        nitroarc_exe,
        '--create',
        '--index-namespace',
        '--file', '@OUTPUT0@',
        '@PRIVATE_DIR@',
    ],
    input : [
        wonder_trade_menu_icon_ncgr,
    ],
    output : [
        'menu_icon.narc',
        'menu_icon.naix',
    ],
)

nitrofs_files += wonder_trade_menu_icon_narc
