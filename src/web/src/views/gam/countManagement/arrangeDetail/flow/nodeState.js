// Keep persisted fields and action templates separate from the editable config.
// Config is JSON data; detach it from imported templates and other node instances.
export const createEmptyNodeConfig = () => ({
  webConfig: { labelList: [], labelFilterList: [], metaDataParams: [], atomic: {} },
  params: []
})

export const createNodeState = (flowItem = {}, action = {}) => {
  const { configObject, ...flowData } = flowItem
  const { configObject: unusedConfig, ...actionDetail } = action
  return {
    flowData,
    actionDetail,
    configObject: JSON.parse(JSON.stringify(configObject ?? createEmptyNodeConfig()))
  }
}

export const updateNodeConfig = (nodes, nodeId, configObject) => {
  if (!nodeId || !configObject) return nodes
  return nodes.map((node) => String(node.id) === String(nodeId)
    ? { ...node, data: { ...node.data, configObject } }
    : node)
}

export const updateAtomicList = (list, payload = {}) => {
  const { position, atomicCode, atomicName, labelList } = payload
  if (!position) return list
  const source = list || []
  const index = source.findIndex((atomic) => String(atomic.position) === String(position))
  const entry = {
    position: String(position),
    atomicCode: atomicCode || '',
    atomicName: atomicName || '',
    labelList: Array.isArray(labelList) ? labelList : []
  }
  return index < 0
    ? [...source, entry]
    : source.map((atomic, current) => current === index ? entry : atomic)
}
