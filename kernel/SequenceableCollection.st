"SequenceableCollection — indexed collection with whileTrue:-based iteration.
 Relies on subclasses providing at:, at:put:, size (typically primitives).
 All iteration uses inlined whileTrue: so outer temp access works."

!SequenceableCollection methodsFor: 'enumerating'!
do: aBlock
    | i sz |
    i := 1.
    sz := self size.
    [i <= sz] whileTrue: [
        aBlock value: (self at: i).
        i := i + 1]!
collect: aBlock
    | result i sz |
    sz := self size.
    result := Array new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (aBlock value: (self at: i)).
        i := i + 1].
    ^ result!
select: aBlock
    | temp count i sz result j |
    sz := self size.
    temp := Array new: sz.
    count := 0.
    i := 1.
    [i <= sz] whileTrue: [
        (aBlock value: (self at: i)) ifTrue: [
            count := count + 1.
            temp at: count put: (self at: i)].
        i := i + 1].
    result := Array new: count.
    j := 1.
    [j <= count] whileTrue: [
        result at: j put: (temp at: j).
        j := j + 1].
    ^ result!
reject: aBlock
    | temp count i sz result j |
    sz := self size.
    temp := Array new: sz.
    count := 0.
    i := 1.
    [i <= sz] whileTrue: [
        (aBlock value: (self at: i)) ifFalse: [
            count := count + 1.
            temp at: count put: (self at: i)].
        i := i + 1].
    result := Array new: count.
    j := 1.
    [j <= count] whileTrue: [
        result at: j put: (temp at: j).
        j := j + 1].
    ^ result!
detect: aBlock ifNone: noneBlock
    | i sz result found |
    i := 1.
    sz := self size.
    found := false.
    [found not and: [i <= sz]] whileTrue: [
        (aBlock value: (self at: i)) ifTrue: [
            result := self at: i.
            found := true].
        i := i + 1].
    found ifFalse: [result := noneBlock value].
    ^ result!
inject: thisValue into: binaryBlock
    "Fold over elements. binaryBlock receives (accumulator, each)."
    | result i |
    result := thisValue.
    i := 1.
    [i <= self size] whileTrue: [
        result := binaryBlock value: result value: (self at: i).
        i := i + 1].
    ^ result!
anySatisfy: aBlock
    "True if aBlock returns true for any element."
    | i result |
    i := 1.
    result := false.
    [i <= self size and: [result not]] whileTrue: [
        (aBlock value: (self at: i)) ifTrue: [result := true].
        i := i + 1].
    ^ result!
allSatisfy: aBlock
    "True if aBlock returns true for every element."
    | i result |
    i := 1.
    result := true.
    [i <= self size and: [result]] whileTrue: [
        (aBlock value: (self at: i)) ifFalse: [result := false].
        i := i + 1].
    ^ result!
reverseDo: aBlock
    | i |
    i := self size.
    [i >= 1] whileTrue: [
        aBlock value: (self at: i).
        i := i - 1]!
with: otherCollection collect: aBlock
    "Collect from two collections in parallel."
    | sz result i |
    sz := self size min: otherCollection size.
    result := Array new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (aBlock value: (self at: i) value: (otherCollection at: i)).
        i := i + 1].
    ^ result!
indexOf: anElement
    | i result |
    i := 1.
    result := 0.
    [i <= self size and: [result = 0]] whileTrue: [
        (self at: i) = anElement ifTrue: [result := i].
        i := i + 1].
    ^ result!
indexOf: anElement ifAbsent: aBlock
    | idx result |
    idx := self indexOf: anElement.
    idx = 0
        ifTrue: [result := aBlock value]
        ifFalse: [result := idx].
    ^ result! !

!SequenceableCollection methodsFor: 'testing'!
includes: anObject
    | found i sz |
    found := false.
    i := 1.
    sz := self size.
    [i <= sz] whileTrue: [
        (self at: i) = anObject ifTrue: [found := true].
        i := i + 1].
    ^ found! !

!SequenceableCollection methodsFor: 'accessing'!
first
    ^ self at: 1!
last
    ^ self at: self size! !

!SequenceableCollection methodsFor: 'converting'!
asArray
    | result i |
    result := Array new: self size.
    i := 1.
    [i <= self size] whileTrue: [
        result at: i put: (self at: i).
        i := i + 1].
    ^ result! !

!SequenceableCollection methodsFor: 'copying'!
copyFrom: start to: stop
    "Return a new Array from start to stop (inclusive)."
    | newSize result i |
    newSize := stop - start + 1.
    newSize < 0 ifTrue: [newSize := 0].
    result := Array new: newSize.
    i := 1.
    [i <= newSize] whileTrue: [
        result at: i put: (self at: start + i - 1).
        i := i + 1].
    ^ result!
copyWith: newElement
    "Return a copy with newElement appended."
    | sz result |
    sz := self size.
    result := Array new: sz + 1.
    result replaceFrom: 1 to: sz with: self startingAt: 1.
    result at: sz + 1 put: newElement.
    ^ result!
copyWithout: anElement
    "Return a copy with all occurrences of anElement removed."
    | count result i j |
    count := 0.
    i := 1.
    [i <= self size] whileTrue: [
        (self at: i) = anElement ifFalse: [count := count + 1].
        i := i + 1].
    result := Array new: count.
    i := 1.
    j := 1.
    [i <= self size] whileTrue: [
        (self at: i) = anElement
            ifFalse: [result at: j put: (self at: i). j := j + 1].
        i := i + 1].
    ^ result! !
