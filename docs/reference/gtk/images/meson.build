ui_files = [
  'aboutdialog.ui',
  'action-bar.ui',
  'button.ui',
  'check-button.ui',
  'color-button.ui',
  'combo-box.ui',
  'combo-box-entry.ui',
  'combo-box-text.ui',
  'drop-down.ui',
  'entry.ui',
  'font-button.ui',
  'info-bar.ui',
  'label.ui',
  'link-button.ui',
  'menu-button.ui',
  'multiline-text.ui',
  'password-entry.ui',
  'radio-button.ui',
  'search-bar.ui',
  'search-entry.ui',
  'separator.ui',
  'switch.ui',
  'toggle-button.ui'
]

gtk_builder_tool = find_program('gtk4-builder-tool')

if get_option('update_screenshots')
  foreach ui_file: ui_files
    png_file = ui_file.replace('.ui', '.png')
    gtk_images += custom_target('@0@ from @1@'.format(png_file, ui_file),
                                input: [ui_file, 'style.css'],
                                output: png_file,
                                command: [ gtk_builder_tool, 'screenshot',
                                           '--force',
                                           '--css', '@INPUT1@',
                                           '@INPUT0@', '@OUTPUT@' ])
  endforeach
endif
