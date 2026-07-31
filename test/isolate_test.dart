// WINDART S3 — dart:isolate validation (V1 / Dart 1.24.3).
// Isolates are load-bearing for the whole workspace (every demo/game/app runs
// in its own isolate). Proves: spawn + one-way result, and a bidirectional
// SendPort/ReceivePort round-trip. Run: dart.exe isolate_test.dart
import 'dart:isolate';
import 'dart:async';

// Worker: compute n*n and send it back on the provided port.
void square(List args) {
  int n = args[0];
  SendPort reply = args[1];
  reply.send(n * n);
}

// Worker: hand back its own SendPort, then echo one message and close.
void echoServer(SendPort initialReplyTo) {
  var port = new ReceivePort();
  initialReplyTo.send(port.sendPort);
  port.listen((msg) {
    var data = msg[0];
    SendPort replyTo = msg[1];
    replyTo.send('echo:$data');
    port.close();
  });
}

main() async {
  // --- one-way: spawn, receive a computed result ---
  var rp = new ReceivePort();
  await Isolate.spawn(square, [7, rp.sendPort]);
  var sq = await rp.first;
  print('isolate square(7) = $sq');

  // --- round-trip: get worker's port, send, await reply ---
  var setup = new ReceivePort();
  await Isolate.spawn(echoServer, setup.sendPort);
  SendPort worker = await setup.first;
  var answer = new ReceivePort();
  worker.send(['ping', answer.sendPort]);
  var reply = await answer.first;
  print('isolate round-trip reply = $reply');

  if (sq == 49 && reply == 'echo:ping') {
    print('ISOLATE_TEST_OK');
  } else {
    print('ISOLATE_TEST_FAIL');
  }
}
