// WINDART S4 vertical-slice probe — the whole GUI framework end-to-end.
// Describes ONE button (+ a label) via dart:win; a click round-trips
// materialize -> WM_COMMAND -> ticket -> _winDispatch -> this closure -> print.
// Run headlessly with WINDART_SELFTEST=1 so the host synthesizes the click.
import 'dart:win';

main() {
  var ui = new Ui.pane(400, 200);
  ui.label('title', text: 'WINDART dart:win vertical slice', frame: [40, 20, 320, 24]);
  ui.button('go', title: 'Click Me', frame: [40, 70, 200, 40], onClick: () {
    print('BUTTON CLICKED (ticket=${ui.ticketOf('go')})');
  });
  var err = ui.commit();
  if (err.isNotEmpty) {
    print('apply error: $err');
  } else {
    print('dartui: surface materialized (button ticket=${ui.ticketOf('go')})');
  }
  uiReady();
}
