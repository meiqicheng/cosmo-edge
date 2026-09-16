import { v4 } from 'uuid'
import { t } from '@/i18n'

function generateActionId() {
  return v4().slice(0, 8)
}

function getLogicalOperations() {
  return [
    { value: 1, label: t('glossary.logicOr') },
    { value: 2, label: t('glossary.logicAnd') },
    { value: 3, label: t('glossary.logicNot') }
  ]
}

function getOperationsMap() {
  return {
    'switch': [
      { value: 11, label: t('glossary.opEqual') }
    ],
    'select': [
      { value: 11, label: t('glossary.opEqual') },
      { value: 12, label: t('glossary.opNotEqual') },
      { value: 13, label: t('glossary.opGreater') },
      { value: 14, label: t('glossary.opGreaterEqual') },
      { value: 15, label: t('glossary.opLess') },
      { value: 16, label: t('glossary.opLessEqual') }
    ],
    select2: [
      { value: 11, label: t('glossary.opEqual') },
      { value: 12, label: t('glossary.opNotEqual') }
    ],
    'check': [
      { value: 31, label: t('glossary.opContains') },
      { value: 32, label: t('glossary.opNotContains') }
    ],
    'radio': [
      { value: 11, label: t('glossary.opEqual') }
    ],
    'other': [
      { value: 11, label: t('glossary.opEqual') },
      { value: 12, label: t('glossary.opNotEqual') },
      { value: 13, label: t('glossary.opGreater') },
      { value: 14, label: t('glossary.opGreaterEqual') },
      { value: 15, label: t('glossary.opLess') },
      { value: 16, label: t('glossary.opLessEqual') }
    ]
  }
}

function getOperations(type) {
  const ops = getOperationsMap()
  switch (type) {
    case 'switch':
      return ops.switch
    case 'select':
      return ops.select
    case 'select2':
      return ops.select2
    case 'check':
      return ops.check
    case 'radio':
      return ops.select
    default:
      return ops.other
  }
}

export { generateActionId, getLogicalOperations, getOperations }
