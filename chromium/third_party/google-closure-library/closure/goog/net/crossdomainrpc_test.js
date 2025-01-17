// Copyright 2007 The Closure Library Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

goog.module('goog.net.CrossDomainRpcTest');
goog.setTestOnly();

const CrossDomainRpc = goog.require('goog.net.CrossDomainRpc');
const GoogPromise = goog.require('goog.Promise');
const TestCase = goog.require('goog.testing.TestCase');
const log = goog.require('goog.log');
const product = goog.require('goog.userAgent.product');
const testSuite = goog.require('goog.testing.testSuite');
const userAgent = goog.require('goog.userAgent');

function print(o) {
  if (Object.prototype.toSource) {
    return o.toSource();
  } else {
    const fragments = [];
    fragments.push('{');
    let first = true;
    for (let p in o) {
      if (!first) fragments.push(',');
      fragments.push(p);
      fragments.push(':"');
      fragments.push(o[p]);
      fragments.push('"');
      first = false;
    }
    return fragments.join('');
  }
}

testSuite({
  setUpPage() {
    TestCase.getActiveTestCase().promiseTimeout = 20000;  // 20s
  },

  testNormalRequest() {
    const start = goog.now();
    return new GoogPromise((resolve, reject) => {
             CrossDomainRpc.send(
                 'crossdomainrpc_test_response.html', resolve, 'POST',
                 {xyz: '01234567891123456789'});
           })
        .then((e) => {
          if (e.target.status < 300) {
            const elapsed = goog.now() - start;
            const responseData = eval(e.target.responseText);
            log.fine(
                CrossDomainRpc.logger_,
                `${elapsed}ms: [` + responseData.result.length + '] ' +
                    print(responseData));
            assertEquals(16 * 1024, responseData.result.length);
            assertEquals(123, e.target.status);
            assertEquals(1, e.target.responseHeaders.a);
            assertEquals('2', e.target.responseHeaders.b);
          } else {
            log.fine(CrossDomainRpc.logger_, print(e));
            fail();
          }
        });
  },

  testErrorRequest() {
    // Firefox and Safari do not give a valid error event.
    if (userAgent.GECKO || product.SAFARI) {
      return;
    }

    return new GoogPromise((resolve, reject) => {
             CrossDomainRpc.send(
                 'http://hoodjimcwaadji.9oo91e.qjz9zk/index.html', resolve, 'POST',
                 {xyz: '01234567891123456789'});
             setTimeout(() => {
               reject('CrossDomainRpc.send did not complete within 4000ms');
             }, 4000);
           })
        .then((e) => {
          if (e.target.status < 300) {
            fail('should have failed requesting a non-existent URI');
          } else {
            log.fine(
                CrossDomainRpc.logger_,
                'expected error seen; event=' + print(e));
          }
        });
  },

  testGetDummyResourceUri() {
    const url = CrossDomainRpc.getDummyResourceUri_();
    assertTrue(
        'dummy resource URL should not contain "?"', url.indexOf('?') < 0);
    assertTrue(
        'dummy resource URL should not contain "#"', url.indexOf('#') < 0);
  },

  testRemoveHash() {
    assertEquals('abc', CrossDomainRpc.removeHash_('abc#123'));
    assertEquals('abc', CrossDomainRpc.removeHash_('abc#12#3'));
  },
});
