export type ThinkingLevel = 'minimal' | 'low' | 'medium' | 'high'

export function liveModel(level: ThinkingLevel) {
  return level === 'minimal' ? 'models/gemini-3.8-live' : 'models/gemini-3.8-live-extended-thinking'
}

export function thinkingConfig(level: ThinkingLevel) {
  return level === 'minimal' ? {} : { thinkingConfig: { thinkingLevel: level.toUpperCase() } }
}

// Extended Thinking can finish several spoken utterances before finishing the
// request. Only IDLE ends the whole interaction; turnComplete alone is a filler.
export function interactionComplete(extended: boolean, turnComplete?: boolean, status?: string) {
  return extended ? status === 'IDLE' : turnComplete === true
}
