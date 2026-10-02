import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import ts from 'typescript'
const { outputText } = ts.transpileModule(readFileSync(new URL('../src/live-model.ts', import.meta.url), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
})
const exports = {}
vm.runInNewContext(outputText, { exports })
const { liveModel, thinkingConfig, interactionComplete } = exports
assert.equal(liveModel('minimal'), 'models/gemini-3.8-live')
assert.equal(JSON.stringify(thinkingConfig('minimal')), '{}')
for (const level of ['low', 'medium', 'high']) {
  assert.equal(liveModel(level), 'models/gemini-3.8-live-extended-thinking')
  assert.equal(thinkingConfig(level).thinkingConfig.thinkingLevel, level.toUpperCase())
}
assert.equal(interactionComplete(false, true), true)
assert.equal(interactionComplete(false, false), false)
assert.equal(interactionComplete(true, true, 'IN_PROGRESS'), false)
assert.equal(interactionComplete(true, true), false)
assert.equal(interactionComplete(true, true, 'IDLE'), true)
assert.equal(interactionComplete(true, undefined, 'IDLE'), true)
console.log('Live modes: model/config selection and filler vs final interaction boundaries passed.')
