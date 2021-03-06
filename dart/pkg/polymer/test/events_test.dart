// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library polymer.test.web.events_test;

import 'dart:html';
import 'package:polymer/polymer.dart';
import 'package:unittest/html_config.dart';
import 'package:unittest/unittest.dart';

@CustomTag("test-b")
class TestB extends PolymerElement {
  TestB.created() : super.created();

  List clicks = [];
  void clickHandler(event, detail, target) {
    clicks.add('local click under $localName (id $id) on ${target.id}');
  }
}

@CustomTag("test-a")
class TestA extends PolymerElement {
  TestA.created() : super.created();

  List clicks = [];
  void clickHandler() {
    clicks.add('host click on: $localName (id $id)');
  }
}

main() {
  initPolymer();
  useHtmlConfiguration();

  setUp(() => Polymer.onReady);

  test('host event', () {
    // Note: this test is currently the only event in
    // polymer/test/js/events.js at commit #7936ff8
    var testA = query('#a');
    expect(testA.clicks, isEmpty);
    testA.click();
    expect(testA.clicks, ['host click on: test-a (id a)']);
  });

  test('local event', () {
    var testB = query('#b');
    expect(testB.clicks, isEmpty);
    testB.click();
    expect(testB.clicks, []);
    var b1 = testB.shadowRoot.query('#b-1');
    b1.click();
    expect(testB.clicks, []);
    var b2 = testB.shadowRoot.query('#b-2');
    b2.click();
    expect(testB.clicks, ['local click under test-b (id b) on b-2']);
  });
}
