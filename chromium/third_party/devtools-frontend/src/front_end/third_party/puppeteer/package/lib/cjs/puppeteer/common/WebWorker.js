"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.WebWorker = void 0;
/**
 * Copyright 2018 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
const EventEmitter_js_1 = require("./EventEmitter.js");
const helper_js_1 = require("./helper.js");
const ExecutionContext_js_1 = require("./ExecutionContext.js");
const JSHandle_js_1 = require("./JSHandle.js");
/**
 * The WebWorker class represents a
 * {@link https://developer.m0z111a.qjz9zk/en-US/docs/Web/API/Web_Workers_API | WebWorker}.
 *
 * @remarks
 * The events `workercreated` and `workerdestroyed` are emitted on the page
 * object to signal the worker lifecycle.
 *
 * @example
 * ```js
 * page.on('workercreated', worker => console.log('Worker created: ' + worker.url()));
 * page.on('workerdestroyed', worker => console.log('Worker destroyed: ' + worker.url()));
 *
 * console.log('Current workers:');
 * for (const worker of page.workers()) {
 *   console.log('  ' + worker.url());
 * }
 * ```
 *
 * @public
 */
class WebWorker extends EventEmitter_js_1.EventEmitter {
    /**
     *
     * @internal
     */
    constructor(client, url, consoleAPICalled, exceptionThrown) {
        super();
        this._client = client;
        this._url = url;
        this._executionContextPromise = new Promise((x) => (this._executionContextCallback = x));
        let jsHandleFactory;
        this._client.once('Runtime.executionContextCreated', async (event) => {
            // eslint-disable-next-line @typescript-eslint/explicit-function-return-type
            jsHandleFactory = (remoteObject) => new JSHandle_js_1.JSHandle(executionContext, client, remoteObject);
            const executionContext = new ExecutionContext_js_1.ExecutionContext(client, event.context, null);
            this._executionContextCallback(executionContext);
        });
        // This might fail if the target is closed before we recieve all execution contexts.
        this._client.send('Runtime.enable').catch(helper_js_1.debugError);
        this._client.on('Runtime.consoleAPICalled', (event) => consoleAPICalled(event.type, event.args.map(jsHandleFactory), event.stackTrace));
        this._client.on('Runtime.exceptionThrown', (exception) => exceptionThrown(exception.exceptionDetails));
    }
    /**
     * @returns The URL of this web worker.
     */
    url() {
        return this._url;
    }
    /**
     * Returns the ExecutionContext the WebWorker runs in
     * @returns The ExecutionContext the web worker runs in.
     */
    async executionContext() {
        return this._executionContextPromise;
    }
    /**
     * If the function passed to the `worker.evaluate` returns a Promise, then
     * `worker.evaluate` would wait for the promise to resolve and return its
     * value. If the function passed to the `worker.evaluate` returns a
     * non-serializable value, then `worker.evaluate` resolves to `undefined`.
     * DevTools Protocol also supports transferring some additional values that
     * are not serializable by `JSON`: `-0`, `NaN`, `Infinity`, `-Infinity`, and
     * bigint literals.
     * Shortcut for `await worker.executionContext()).evaluate(pageFunction, ...args)`.
     *
     * @param pageFunction - Function to be evaluated in the worker context.
     * @param args - Arguments to pass to `pageFunction`.
     * @returns Promise which resolves to the return value of `pageFunction`.
     */
    async evaluate(pageFunction, ...args) {
        return (await this._executionContextPromise).evaluate(pageFunction, ...args);
    }
    /**
     * The only difference between `worker.evaluate` and `worker.evaluateHandle`
     * is that `worker.evaluateHandle` returns in-page object (JSHandle). If the
     * function passed to the `worker.evaluateHandle` returns a [Promise], then
     * `worker.evaluateHandle` would wait for the promise to resolve and return
     * its value. Shortcut for
     * `await worker.executionContext()).evaluateHandle(pageFunction, ...args)`
     *
     * @param pageFunction - Function to be evaluated in the page context.
     * @param args - Arguments to pass to `pageFunction`.
     * @returns Promise which resolves to the return value of `pageFunction`.
     */
    async evaluateHandle(pageFunction, ...args) {
        return (await this._executionContextPromise).evaluateHandle(pageFunction, ...args);
    }
}
exports.WebWorker = WebWorker;
