"OrderedCollection — variable-size ordered collection with managed backing array."

SequenceableCollection subclass: #OrderedCollection
    instanceVariableNames: 'array firstIndex lastIndex'
    classVariableNames: ''
    poolDictionaries: ''
    category: 'Collections-Sequenceable'

!OrderedCollection methodsFor: 'initialization'!
initialize
    array := Array new: 4.
    firstIndex := 1.
    lastIndex := 0! !

!OrderedCollection methodsFor: 'accessing'!
size
    ^ lastIndex - firstIndex + 1!
at: anIndex
    | realIndex |
    realIndex := firstIndex + anIndex - 1.
    ^ array at: realIndex!
at: anIndex put: anObject
    | realIndex |
    realIndex := firstIndex + anIndex - 1.
    array at: realIndex put: anObject.
    ^ anObject! !

!OrderedCollection methodsFor: 'adding'!
add: anObject
    "Add to the end. Grow if necessary."
    lastIndex = array size ifTrue: [self grow].
    lastIndex := lastIndex + 1.
    array at: lastIndex put: anObject.
    ^ anObject!
addFirst: anObject
    "Add to the beginning. Grow if necessary."
    firstIndex = 1 ifTrue: [self growAtFront].
    firstIndex := firstIndex - 1.
    array at: firstIndex put: anObject.
    ^ anObject! !

!OrderedCollection methodsFor: 'removing'!
removeFirst
    | result |
    self size = 0 ifTrue: [self error: 'collection is empty'].
    result := array at: firstIndex.
    array at: firstIndex put: nil.
    firstIndex := firstIndex + 1.
    ^ result!
removeLast
    | result |
    self size = 0 ifTrue: [self error: 'collection is empty'].
    result := array at: lastIndex.
    array at: lastIndex put: nil.
    lastIndex := lastIndex - 1.
    ^ result! !

!OrderedCollection methodsFor: 'enumerating'!
do: aBlock
    firstIndex to: lastIndex do: [:i |
        aBlock value: (array at: i)]! !

!OrderedCollection methodsFor: 'private'!
grow
    "Double the backing array."
    | sz newArray |
    sz := self size.
    newArray := Array new: array size * 2.
    newArray replaceFrom: 1 to: sz with: array startingAt: firstIndex.
    array := newArray.
    firstIndex := 1.
    lastIndex := sz!
growAtFront
    "Grow and shift elements to make room at the front."
    | sz newSize newArray offset |
    sz := self size.
    newSize := array size * 2.
    newArray := Array new: newSize.
    offset := newSize - array size.
    newArray replaceFrom: offset + firstIndex to: offset + lastIndex with: array startingAt: firstIndex.
    firstIndex := offset + firstIndex.
    lastIndex := offset + lastIndex.
    array := newArray! !

!OrderedCollection methodsFor: 'printing'!
printString
    | result |
    result := 'an OrderedCollection('.
    1 to: self size do: [:i |
        i > 1 ifTrue: [result := result, ' '].
        result := result, (self at: i) printString].
    result := result, ')'.
    ^ result! !

!OrderedCollection methodsFor: 'converting'!
asArray
    | result |
    result := Array new: self size.
    1 to: self size do: [:i |
        result at: i put: (self at: i)].
    ^ result! !
