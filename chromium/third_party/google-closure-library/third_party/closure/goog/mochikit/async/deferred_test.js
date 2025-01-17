/**
 * Copyright 2009 The Closure Library Authors. All Rights Reserved.
 * Author: arv@9oo91e.qjz9zk (Erik Arvidsson)
 */

goog.module('goog.async.deferredTest');
goog.setTestOnly();
const Deferred = goog.require('goog.async.Deferred');
const GoogPromise = goog.require('goog.Promise');
const GoogThenable = goog.require('goog.Thenable');
const MockClock = goog.require('goog.testing.MockClock');
const PropertyReplacer = goog.require('goog.testing.PropertyReplacer');
const recordFunction = goog.require('goog.testing.recordFunction');
const testSuite = goog.require('goog.testing.testSuite');

var AlreadyCalledError = Deferred.AlreadyCalledError;
var CanceledError = Deferred.CanceledError;

// Unhandled errors may be sent to the browser on a timeout.
var mockClock = new MockClock();
var stubs = new PropertyReplacer();

/**
 * @param {string} msg Message to show upon failure.
 * @param {T} expected Expected value.
 * @return {function(T): T} Function to be called with the actual value that
 *     will assert that it is equal to the expected value.
 * @template T
 */
function assertEqualsCallback(msg, expected) {
  return function(res) {
    assertEquals(msg, expected, res);
    // Since the assertion is an exception that will be caught inside the
    // Deferred object, we must advance the clock to see if it has failed.
    mockClock.tick();
    return res;
  };
}

/**
 * @param {number} res
 * @return {number}
 */
function increment(res) {
  return res + 1;
}

/**
 * @param {*} res
 */
function throwStuff(res) {
  throw res;
}

/**
 * @param {*} res
 * @return {*}
 */
function catchStuff(res) {
  return res;
}

/**
 * @param {*} res
 * @return {!Error}
 */
function returnError(res) {
  return Error(res);
}

/**
 * @param {?} res
 */
function neverHappen(res) {
  fail('This should not happen');
}

testSuite({
  setUp() {
    mockClock.install();
  },

  tearDown() {
    // Advance the mockClock to fire any unhandled exception timeouts.
    mockClock.tick();
    mockClock.uninstall();
    stubs.reset();
  },

  testNormal() {
    var d = new Deferred();
    d.addCallback(assertEqualsCallback('pre-deferred callback', 1));
    d.callback(1);
    d.addCallback(increment);
    d.addCallback(assertEqualsCallback('post-deferred callback', 2));
    d.addCallback(throwStuff);
    d.addCallback(neverHappen);
    d.addErrback(catchStuff);
    d.addCallback(assertEqualsCallback('throw -> err, catch -> success', 2));
    d.addCallback(returnError);
    d.addCallback(neverHappen);
    d.addErrback(catchStuff);
    d.addCallback(assertEqualsCallback('return -> err, catch -> succcess', 2));
  },

  testCancel() {
    var count = 0;
    function canceled(d) {
      count++;
    }

    function canceledError(res) {
      assertTrue(res instanceof CanceledError);
    }

    var d = new Deferred(canceled);
    d.addCallback(neverHappen);
    d.addErrback(canceledError);
    d.cancel();

    assertEquals(1, count);
  },

  testSucceedFail() {
    var count = 0;

    var d = Deferred.succeed(1).addCallback(assertEqualsCallback('succeed', 1));

    // default error
    d = Deferred.fail().addCallback(neverHappen);
    d = d.addErrback(function(res) {
      count++;
      return res;
    });

    // default wrapped error
    d = Deferred.fail('web taco')
            .addCallback(neverHappen)
            .addErrback(catchStuff);
    d = d.addCallback(assertEqualsCallback('wrapped fail', 'web taco'));

    // default unwrapped error
    d = Deferred.fail(Error('ugh'))
            .addCallback(neverHappen)
            .addErrback(catchStuff);
    d = d.addCallback(assertEqualsCallback('unwrapped fail', 'ugh'));

    assertEquals(1, count);
  },

  testDeferredDependencies() {
    function deferredIncrement(res) {
      var rval = Deferred.succeed(res);
      rval.addCallback(increment);
      return rval;
    }

    var d = Deferred.succeed(1).addCallback(deferredIncrement);
    d = d.addCallback(assertEqualsCallback('dependent deferred succeed', 2));

    function deferredFailure(res) {
      return Deferred.fail(res);
    }

    d = Deferred.succeed('ugh')
            .addCallback(deferredFailure)
            .addErrback(catchStuff);
    d = d.addCallback(assertEqualsCallback('dependent deferred fail', 'ugh'));
  },

  // Test double-calling, double-failing, etc.
  testDoubleCalling() {
    var ex = assertThrows(function() {
      Deferred.succeed(1).callback(2);
      neverHappen();
    });
    assertTrue('double call', ex instanceof AlreadyCalledError);
  },

  testDoubleCalling2() {
    var ex = assertThrows(function() {
      Deferred.fail(1).errback(2);
      neverHappen();
    });
    assertTrue('double-fail', ex instanceof AlreadyCalledError);
  },

  testDoubleCalling3() {
    var ex = assertThrows(function() {
      var d = Deferred.succeed(1);
      d.cancel();
      d = d.callback(2);
      assertTrue('swallowed one callback, no canceler', true);
      d.callback(3);
      neverHappen();
    });
    assertTrue('swallow cancel', ex instanceof AlreadyCalledError);
  },

  testDoubleCalling4() {
    var count = 0;
    function canceled(d) {
      count++;
    }

    var ex = assertThrows(function() {
      var d = new Deferred(canceled);
      d.cancel();
      d = d.callback(1);
    });

    assertTrue('non-swallowed cancel', ex instanceof AlreadyCalledError);
    assertEquals(1, count);
  },

  // Test incorrect Deferred usage
  testIncorrectUsage() {
    var d = new Deferred();

    var ex = assertThrows(function() {
      d.callback(new Deferred());
      neverHappen();
    });
    assertTrue('deferred not allowed for callback', ex instanceof Error);
  },

  testIncorrectUsage2() {
    var d = new Deferred();

    var ex = assertThrows(function() {
      d.errback(new Deferred());
      neverHappen();
    });
    assertTrue('deferred not allowed for errback', ex instanceof Error);
  },

  testIncorrectUsage3() {
    var d = new Deferred();
    (new Deferred())
        .addCallback(function() {
          return d;
        })
        .callback(1);

    var ex = assertThrows(function() {
      d.addCallback(function() {});
      neverHappen();
    });
    assertTrue(
        'chained deferred not allowed to be re-used', ex instanceof Error);
  },

  testCallbackScope1() {
    var c1 = {}, c2 = {};
    var callbackScope = null;
    var errbackScope = null;

    var d = new Deferred();
    d.addCallback(function() {
      callbackScope = this;
      throw Error('Foo');
    }, c1);
    d.addErrback(function() {
      errbackScope = this;
    }, c2);
    d.callback();
    assertEquals('Incorrect callback scope', c1, callbackScope);
    assertEquals('Incorrect errback scope', c2, errbackScope);
  },

  testCallbackScope2() {
    var c = {};
    var callbackScope = null;
    var errbackScope = null;

    var d = new Deferred(null, c);
    d.addCallback(function() {
      callbackScope = this;
      throw Error('Foo');
    });
    d.addErrback(function() {
      errbackScope = this;
    });
    d.callback();
    assertEquals('Incorrect callback scope', c, callbackScope);
    assertEquals('Incorrect errback scope', c, errbackScope);
  },

  testChainedDeferred1() {
    var calls = [];

    var d2 = new Deferred();
    d2.addCallback(function() {
      calls.push('B1');
    });
    d2.addCallback(function() {
      calls.push('B2');
    });

    var d1 = new Deferred();
    d1.addCallback(function() {
      calls.push('A1');
    });
    d1.addCallback(function() {
      calls.push('A2');
    });
    d1.chainDeferred(d2);
    d1.addCallback(function() {
      calls.push('A3');
    });

    d1.callback();
    assertEquals('A1,A2,B1,B2,A3', calls.join(','));
  },

  testChainedDeferred2() {
    var calls = [];

    var d2 = new Deferred();
    d2.addCallback(function() {
      calls.push('B1');
    });
    d2.addErrback(function(err) {
      calls.push('B2');
      throw Error('x');
    });

    var d1 = new Deferred();
    d1.addCallback(function(err) {
      throw Error('foo');
    });
    d1.chainDeferred(d2);
    d1.addCallback(function() {
      calls.push('A1');
    });
    d1.addErrback(function() {
      calls.push('A2');
    });

    d1.callback();
    assertEquals('B2,A2', calls.join(','));

    var ex = assertThrows(function() {
      mockClock.tick();
      neverHappen();
    });
    assertTrue('Should catch unhandled throw from d2.', ex.message == 'x');
  },

  testUndefinedResultAndCallbackSequence() {
    var results = [];
    var d = new Deferred();
    d.addCallback(function(res) {
      return 'foo';
    });
    d.addCallback(function(res) {
      results.push(res);
      return 'bar';
    });
    d.addCallback(function(res) {
      results.push(res);
    });
    d.addCallback(function(res) {
      results.push(res);
    });
    d.callback();
    assertEquals('foo,bar,bar', results.join(','));
  },

  testUndefinedResultAndErrbackSequence() {
    var results = [];
    var d = new Deferred();
    d.addCallback(function(res) {
      throw Error('uh oh');
    });
    d.addErrback(function(res) {
      results.push('A');
    });
    d.addCallback(function(res) {
      results.push('B');
    });
    d.addErrback(function(res) {
      results.push('C');
    });
    d.callback();
    assertEquals('A,C', results.join(','));
  },

  testHasFired() {
    var d1 = new Deferred();
    var d2 = new Deferred();

    assertFalse(d1.hasFired());
    assertFalse(d2.hasFired());

    d1.callback();
    d2.errback();
    assertTrue(d1.hasFired());
    assertTrue(d2.hasFired());
  },

  testUnhandledErrors() {
    var d = new Deferred();
    d.addCallback(throwStuff);

    var ex = assertThrows(function() {
      d.callback(123);
      mockClock.tick();
      neverHappen();
    });
    assertEquals('Unhandled throws should hit the browser.', 123, ex);

    assertNotThrows(
        'Errbacks added after a failure should resume.', function() {
          d.addErrback(catchStuff);
          mockClock.tick();
        });

    d.addCallback(assertEqualsCallback('Should recover after throw.', 1));
    mockClock.tick();
  },

  testStrictUnhandledErrors() {
    stubs.replace(Deferred, 'STRICT_ERRORS', true);
    var err = Error('never handled');

    // The registered errback exists, but doesn't modify the error value.
    var d = Deferred.succeed();
    d.addCallback(function(res) {
      throw err;
    });
    d.addErrback(function(unhandledErr) {});

    var caught = assertThrows(
        'The error should be rethrown at the next clock tick.', function() {
          mockClock.tick();
        });
    assertEquals(err, caught);
  },

  testStrictHandledErrors() {
    stubs.replace(Deferred, 'STRICT_ERRORS', true);

    // The registered errback returns a non-error value.
    var d = Deferred.succeed();
    d.addCallback(function(res) {
      throw Error('eventually handled');
    });
    d.addErrback(function(unhandledErr) {
      return true;
    });

    assertNotThrows(
        'The error was handled and should not be rethrown', function() {
          mockClock.tick();
        });
    d.addCallback(function(res) {
      assertTrue(res);
    });
  },

  testStrictBlockedErrors() {
    stubs.replace(Deferred, 'STRICT_ERRORS', true);

    var d1 = Deferred.fail(Error('blocked failure'));
    var d2 = new Deferred();

    d1.addBoth(function() {
      return d2;
    });
    assertNotThrows('d1 should be blocked until d2 fires.', function() {
      mockClock.tick();
    });

    d2.callback('unblocked');
    d1.addCallback(assertEqualsCallback(
        'd1 should receive the fired result from d2.', 'unblocked'));
  },

  testStrictCanceledErrors() {
    stubs.replace(Deferred, 'STRICT_ERRORS', true);

    Deferred.canceled();
    assertNotThrows(
        'CanceledErrors should not be rethrown to the global scope.',
        function() {
          mockClock.tick();
        });
  },

  testSynchronousErrorCanceling() {
    var d = new Deferred();
    d.addCallback(throwStuff);

    assertNotThrows(
        'Adding an errback to the end of a failing Deferred should cancel the ' +
            'unhandled error timeout.',
        function() {
          d.callback(1);
          d.addErrback(catchStuff);
          mockClock.tick();
        });

    d.addCallback(assertEqualsCallback('Callback should fire', 1));
  },

  testThrowNonError() {
    var results = [];

    var d = new Deferred();
    d.addCallback(function(res) {
      throw res;
    });
    d.addErrback(function(res) {
      results.push(res);
      return 6;
    });
    d.addCallback(function(res) {
      results.push(res);
    });

    d.callback(7);
    assertArrayEquals(
        'Errback should have been called with 7, followed by callback with 6.',
        [7, 6], results);
  },

  testThrownErrorWithNoErrbacks() {
    var d = new Deferred();
    d.addCallback(function() {
      throw Error('foo');
    });
    d.addCallback(goog.nullFunction);

    function assertCallback() {
      d.callback(1);
      mockClock.tick();  // Should cause error because throwing is delayed.
    }

    assertThrows(
        'A thrown error should be rethrown if there is no ' +
            'errback to catch it.',
        assertCallback);
  },

  testThrownErrorCallbacksDoNotCancel() {
    var d = new Deferred();
    d.addCallback(function() {
      throw Error('foo');
    });

    function assertCallback() {
      d.callback(1);
      // Add another callback after the fact.  Note this is not an errback!
      d.addCallback(neverHappen);
      mockClock.tick();  // Should cause error because throwing is delayed.
    }

    assertThrows(
        'A thrown error should be rethrown if there is no ' +
            'errback to catch it.',
        assertCallback);
  },

  testAwaitDeferred() {
    var results = [];

    function fn(x) {
      return function() {
        results.push(x);
      };
    }

    var d2 = new Deferred();
    d2.addCallback(fn('b'));

    // d1 -> a -> (wait for d2) -> c
    var d1 = new Deferred();
    d1.addCallback(fn('a'));
    d1.awaitDeferred(d2);
    d1.addCallback(fn('c'));

    // calls 'a' then yields for d2.
    d1.callback(null);

    // will get called after d2.
    d1.addCallback(fn('d'));

    assertEquals('a', results.join(''));

    // d3 -> w -> (wait for d2) -> x
    var d3 = new Deferred();
    d3.addCallback(fn('w'));
    d3.awaitDeferred(d2);
    d3.addCallback(fn('x'));

    // calls 'w', then yields for d2.
    d3.callback();


    // will get called after d2.
    d3.addCallback(fn('y'));

    assertEquals('aw', results.join(''));

    // d1 calls 'd', d3 calls 'y'
    d2.callback(null);

    assertEquals('awbcdxy', results.join(''));

    // d3 and d2 already called, so 'z' called immediately.
    d3.addCallback(fn('z'));

    assertEquals('awbcdxyz', results.join(''));
  },

  testAwaitDeferred_withPromise() {
    var results = [];

    function fn(x) {
      return function() {
        results.push(x);
      };
    }

    var resolver = new GoogPromise.withResolver();
    resolver.promise.then(fn('b'));

    // d1 -> a -> (wait for promise) -> c
    var d1 = new Deferred();
    d1.addCallback(fn('a'));
    d1.awaitDeferred(resolver.promise);
    d1.addCallback(fn('c'));

    // calls 'a' then yields for promise.
    d1.callback(1);

    // will get called after promise.
    d1.addCallback(fn('d'));

    assertEquals('a', results.join(''));

    // d3 -> w -> (wait for promise) -> x
    var d3 = new Deferred();
    d3.addCallback(fn('w'));
    d3.awaitDeferred(resolver.promise);
    d3.addCallback(fn('x'));

    // calls 'w', then yields for promise.
    d3.callback(2);


    // will get called after promise.
    d3.addCallback(fn('y'));

    assertEquals('aw', results.join(''));

    // d1 calls 'd', d3 calls 'y'
    resolver.resolve();
    mockClock.tick();

    assertEquals('awbcdxy', results.join(''));

    // d3 and promise already called, so 'z' called immediately.
    d3.addCallback(fn('z'));

    assertEquals('awbcdxyz', results.join(''));
  },

  testAwaitDeferredWithErrors() {
    var results = [];

    function fn(x) {
      return function(e) {
        results.push(x);
      };
    }

    var d2 = new Deferred();
    d2.addErrback(fn('a'));

    var d1 = new Deferred();
    d1.awaitDeferred(d2);
    d1.addCallback(fn('x'));
    d1.addErrback(fn('b'));
    d1.callback(null);

    assertEquals('', results.join(''));

    d2.addCallback(fn('z'));
    d2.addErrback(fn('c'));
    d2.errback(null);

    // First errback added to d2 prints 'a'.
    // Next 'd' was chained, so execute its err backs, printing 'b'.
    // Finally 'c' was added last by d2's errback.
    assertEquals('abc', results.join(''));
  },

  testNonErrorErrback() {
    var results = [];

    function fn(x) {
      return function(e) {
        results.push(x);
      };
    }

    var d = new Deferred();
    d.addCallback(fn('a'));
    d.addErrback(fn('b'));

    d.addCallback(fn('c'));
    d.addErrback(fn('d'));

    d.errback('foo');

    assertEquals('bd', results.join(''));
  },

  testUnequalReturnValueForErrback() {
    var results = [];

    function fn(x) {
      return function(e) {
        results.push(x);
      };
    }

    var d = new Deferred();
    d.addCallback(fn('a'));
    d.addErrback(function() {
      results.push('b');
      return 'bar';
    });

    d.addCallback(fn('c'));
    d.addErrback(fn('d'));

    d.errback('foo');

    assertEquals('bc', results.join(''));
  },

  testBranch() {
    function fn(x) {
      return function(arr) {
        return arr.concat(x);
      };
    }

    var d = new Deferred();
    d.addCallback(fn(1));
    d.addCallback(fn(2));
    var d2 = d.branch();
    d.addCallback(fn(3));
    d2.addCallback(fn(4));

    d.callback([]);

    assertTrue('both deferreds should have fired', d.hasFired());
    assertTrue('both deferreds should have fired', d2.hasFired());
    d.addCallback(function(arr) {
      assertArrayEquals([1, 2, 3], arr);
    });
    d2.addCallback(function(arr) {
      assertArrayEquals([1, 2, 4], arr);
    });
  },

  testDiamondBranch() {
    function fn(x) {
      return function(arr) {
        return arr.concat(x);
      };
    }

    var d = new Deferred();
    d.addCallback(fn(1));

    var d2 = d.branch();
    d2.addCallback(fn(2));

    // Chain the branch back to the original. There is no good reason to do this
    // cever.
    d.addCallback(function(ret) {
      return d2;
    });
    d.callback([]);

    // But no reason it shouldn't work!
    d.addCallback(function(arr) {
      assertArrayEquals([1, 2], arr);
    });
  },

  testRepeatedBranch() {
    var d = new Deferred().addCallback(increment);

    d.branch()
        .addCallback(
            assertEqualsCallback('branch should be after increment', 2))
        .addCallback(function(res) {
          return d.branch();
        })
        .addCallback(
            assertEqualsCallback('second branch should be the same', 2));
    d.callback(1);
  },

  testCancelThroughBranch() {
    var wasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    var branch1 = d.branch(true);
    var branch2 = d.branch(true);

    branch1.cancel();
    assertFalse(wasCanceled);
    branch2.cancel();
    assertTrue(wasCanceled);
  },

  testCancelThroughSeveralBranches() {
    var wasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    var branch = d.branch(true).branch(true).branch(true);

    branch.cancel();
    assertTrue(wasCanceled);
  },

  testBranchCancelThenCallback() {
    var wasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    var wasCalled = false;
    d.addCallback(function() {
      wasCalled = true;
    });
    var branch1 = d.branch();
    var branch2 = d.branch();

    var branch1WasCalled = false;
    var branch2WasCalled = false;
    branch1.addCallback(function() {
      branch1WasCalled = true;
    });
    branch2.addCallback(function() {
      branch2WasCalled = true;
    });

    var branch1HadErrback = false;
    var branch2HadErrback = false;
    branch1.addErrback(function() {
      branch1HadErrback = true;
    });
    branch2.addErrback(function() {
      branch2HadErrback = true;
    });

    branch1.cancel();
    assertFalse(wasCanceled);
    assertTrue(branch1HadErrback);
    assertFalse(branch2HadErrback);

    d.callback();
    assertTrue(wasCalled);
    assertFalse(branch1WasCalled);
    assertTrue(branch2WasCalled);
  },

  testDeepCancelOnBranch() {
    var wasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    var branch1 = d.branch(true);
    var branch2 = d.branch(true).branch(true).branch(true);

    var branch1HadErrback = false;
    var branch2HadErrback = false;
    branch1.addErrback(function() {
      branch1HadErrback = true;
    });
    branch2.addErrback(function() {
      branch2HadErrback = true;
    });

    branch2.cancel(true /* opt_deepCancel */);
    assertTrue(wasCanceled);
    assertTrue(branch1HadErrback);
    assertTrue(branch2HadErrback);
  },

  testCancelOnRoot() {
    var wasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    d.branch(true).branch(true).branch(true);

    d.cancel();
    assertTrue(wasCanceled);
  },

  testCancelOnLeafBranch() {
    var wasCanceled = false;
    var branchWasCanceled = false;
    var d = new Deferred(function() {
      wasCanceled = true;
    });
    var branch = d.branch(true).branch(true).branch(true);
    branch.addErrback(function() {
      branchWasCanceled = true;
    });

    branch.cancel();
    assertTrue(wasCanceled);
    assertTrue(branchWasCanceled);
  },

  testCancelOnIntermediateBranch() {
    var rootWasCanceled = false;

    var d = new Deferred(function() {
      rootWasCanceled = true;
    });
    var branch = d.branch(true).branch(true).branch(true);

    var deepBranch1 = branch.branch(true);
    var deepBranch2 = branch.branch(true);

    branch.cancel();
    assertTrue(rootWasCanceled);
    assertTrue(deepBranch1.hasFired());
    assertTrue(deepBranch2.hasFired());
  },

  testCancelWithSomeCompletedBranches() {
    var d = new Deferred();
    var branch1 = d.branch(true);

    var branch1HadCallback = false;
    var branch1HadErrback = false;
    branch1
        .addCallback(function() {
          branch1HadCallback = true;
        })
        .addErrback(function() {
          branch1HadErrback = true;
        });
    d.callback(true);

    assertTrue(branch1HadCallback);
    assertFalse(branch1HadErrback);

    var rootHadCallback = false;
    var rootHadErrback = false;
    // Block the root on a new Deferred indefinitely.
    d.addCallback(function() {
       rootHadCallback = true;
     })
        .addCallback(function() {
          return new Deferred();
        })
        .addErrback(function() {
          rootHadErrback = true;
        });
    var branch2 = d.branch(true);

    assertTrue(rootHadCallback);
    assertFalse(rootHadErrback);

    branch2.cancel();
    assertFalse(branch1HadErrback);
    assertTrue(
        'Canceling the last active branch should cancel the parent.',
        rootHadErrback);
  },

  testStaticCanceled() {
    var callbackCalled = false;
    var errbackResult = null;

    var d = Deferred.canceled();
    d.addCallback(function() {
      callbackCalled = true;
    });
    d.addErrback(function(err) {
      errbackResult = err;
    });

    assertTrue(
        'Errback should have been called with a canceled error',
        errbackResult instanceof Deferred.CanceledError);
    assertFalse('Callback should not have been called', callbackCalled);
  },

  testWhenWithValues() {
    var called = false;
    Deferred.when(4, function(obj) {
      called = true;
      assertEquals(4, obj);
    });
    assertTrue('Fn should have been called', called);
  },

  testWhenWithDeferred() {
    var called = false;

    var d = new Deferred();
    Deferred.when(d, function(obj) {
      called = true;
      assertEquals(6, obj);
    });
    assertFalse('Fn should not have been called yet', called);
    d.callback(6);
    assertTrue('Fn should have been called', called);
  },

  testWhenDoesntAlterOriginalChain() {
    var calls = 0;

    var d1 = new Deferred();
    var d2 = Deferred.when(d1, function(obj) {
      calls++;
      return obj * 2;
    });
    d1.addCallback(function(obj) {
      assertEquals('Original chain should get original value', 5, obj);
      calls++;
    });
    d2.addCallback(function(obj) {
      assertEquals('Branched chain should get modified value', 10, obj);
      calls++;
    });

    d1.callback(5);

    assertEquals('There should have been 3 callbacks', 3, calls);
  },

  testAssertNoErrors() {
    var d = new Deferred();
    d.addCallback(function() {
      throw new Error('Foo');
    });
    d.callback(1);

    var ex = assertThrows(function() {
      Deferred.assertNoErrors();
      neverHappen();
    });
    assertEquals('Expected to get thrown error', 'Foo', ex.message);

    assertNotThrows(
        'Calling Deferred.assertNoErrors() a second time with only one ' +
            'scheduled error should pass.',
        function() {
          Deferred.assertNoErrors();
        });
  },

  testThen() {
    var result;
    var result2;
    var d = new Deferred();
    assertEquals(d.then, d['then']);
    d.then(function(r) {
       return result = r;
     }).then(function(r2) {
      result2 = r2;
    });
    d.callback('done');
    assertUndefined(result);
    mockClock.tick();
    assertEquals('done', result);
    assertEquals('done', result2);
  },

  testThen_reject() {
    var result, error;
    var d = new Deferred();
    assertEquals(d.then, d['then']);
    d.then(
        function(r) {
          result = r;
        },
        function(e) {
          error = e;
        });
    d.errback(new Error('boom'));
    assertUndefined(result);
    mockClock.tick();
    assertUndefined(result);
    assertEquals('boom', error.message);
  },

  testPromiseAll() {
    var d = new Deferred();
    var p = new GoogPromise(function(resolve) {
      resolve('promise');
    });
    GoogPromise.all([d, p]).then(function(values) {
      assertEquals(2, values.length);
      assertEquals('deferred', values[0]);
      assertEquals('promise', values[1]);
    });
    d.callback('deferred');
    mockClock.tick();
  },

  testGoogPromiseBlocksDeferred() {
    var result;
    var d = new Deferred();
    var p = new GoogPromise(function(resolve) {
      resolve('promise');
    });
    d.callback();
    d.addCallback(function() {
      return p;
    });
    d.addCallback(function(r) {
      result = r;
    });

    assertUndefined(result);
    mockClock.tick();
    assertEquals('promise', result);
  },

  testNativePromiseBlocksDeferred() {
    // Early-out for browsers that don't support native Promises.
    if (typeof Promise !== 'function') {
      return;
    }

    // Disable mock clock for this test.  Native promises don't abide by it, so
    // it just complicates things in this case.
    mockClock.uninstall();

    var blockedOnPromise = true;

    var resolver;
    var d = Deferred.succeed();
    d.addCallback(function() {
      return new Promise(function(resolve) {
        resolver = resolve;
      });
    });
    d.addCallback(function(r) {
      blockedOnPromise = false;
      assertEquals('Native promise value', 'result', r);
    });

    // Verify that the callback chain executes up to, but not beyond, the
    // callback that returns a Promise.
    assertTrue(!!resolver);
    assertTrue(
        'Deferred chain not blocked on Promise resolution', blockedOnPromise);
    // Resolve the promise, which should unblock the rest of the callback chain.
    resolver('result');
    return d;
  },

  testFromPromiseWithGoogPromise() {
    var result;
    var p = new GoogPromise(function(resolve) {
      resolve('promise');
    });
    var d = Deferred.fromPromise(p);
    d.addCallback(function(value) {
      result = value;
    });
    assertUndefined(result);
    mockClock.tick();
    assertEquals('promise', result);
  },

  testFromPromiseWithThenable() {
    var result;
    var p = {
      'then': function(callback) {
        callback('promise');
      }
    };
    var d = Deferred.fromPromise(p);
    d.addCallback(function(value) {
      result = value;
    });
    assertEquals('promise', result);
  },

  testPromiseBlocksDeferredAndRejects() {
    var result;
    var d = new Deferred();
    var p = new GoogPromise(function(resolve, reject) {
      reject(new Error('error'));
    });
    d.callback();
    d.addCallback(function(r) {
      return p;
    });
    d.addErrback(function(r) {
      result = r;
    });

    assertUndefined(result);
    mockClock.tick();
    assertEquals('error', result.message);
  },

  testPromiseFromCanceledDeferred() {
    var result;
    var d = new Deferred();
    d.cancel();

    d.then(neverHappen, function(reason) {
      result = reason;
    });

    mockClock.tick();
    assertTrue(result instanceof GoogPromise.CancellationError);
  },

  testThenableInterface() {
    var d = new Deferred();
    assertTrue(GoogThenable.isImplementedBy(d));
  },

  testAddBothPropagatesToErrback() {
    var log = [];
    var deferred = new Deferred();
    deferred.addBoth(goog.nullFunction);
    deferred.addErrback(function() {
      log.push('errback');
    });
    deferred.errback(new Error('my error'));

    mockClock.tick(1);
    assertArrayEquals(['errback'], log);
  },

  testAddBothDoesNotPropagateUncaughtExceptions() {
    var deferred = new Deferred();
    deferred.addBoth(goog.nullFunction);
    deferred.errback(new Error('my error'));
    mockClock.tick(1);
  },

  testAddFinally() {
    var deferred = new Deferred();
    var callback = recordFunction();
    var thisArg = {};
    deferred.addFinally(callback, thisArg);
    deferred.errback(new Error('my error'));

    try {
      mockClock.tick(1);
    } catch (e) {
      assertEquals('my error', e.message);
    }
    assertEquals(thisArg, callback.getCalls()[0].getThis());
  }
});
