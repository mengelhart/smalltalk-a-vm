"SequenceableCollection — indexed collection with idiomatic iteration.
 Relies on subclasses providing at:, at:put:, size (typically primitives)."

!SequenceableCollection methodsFor: 'enumerating'!
do: aBlock
    1 to: self size do: [:i | aBlock value: (self at: i)]!
collect: aBlock
    | result sz |
    sz := self size.
    result := Array new: sz.
    1 to: sz do: [:i |
        result at: i put: (aBlock value: (self at: i))].
    ^ result!
select: aBlock
    | temp count sz result |
    sz := self size.
    temp := Array new: sz.
    count := 0.
    1 to: sz do: [:i |
        (aBlock value: (self at: i)) ifTrue: [
            count := count + 1.
            temp at: count put: (self at: i)]].
    result := Array new: count.
    1 to: count do: [:j |
        result at: j put: (temp at: j)].
    ^ result!
reject: aBlock
    | temp count sz result |
    sz := self size.
    temp := Array new: sz.
    count := 0.
    1 to: sz do: [:i |
        (aBlock value: (self at: i)) ifFalse: [
            count := count + 1.
            temp at: count put: (self at: i)]].
    result := Array new: count.
    1 to: count do: [:j |
        result at: j put: (temp at: j)].
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
    | result |
    result := thisValue.
    1 to: self size do: [:i |
        result := binaryBlock value: result value: (self at: i)].
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
    | sz result |
    sz := self size min: otherCollection size.
    result := Array new: sz.
    1 to: sz do: [:i |
        result at: i put: (aBlock value: (self at: i) value: (otherCollection at: i))].
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
    | result |
    result := Array new: self size.
    1 to: self size do: [:i |
        result at: i put: (self at: i)].
    ^ result! !

!SequenceableCollection methodsFor: 'copying'!
copyFrom: start to: stop
    "Return a new Array from start to stop (inclusive)."
    | newSize result |
    newSize := stop - start + 1.
    newSize < 0 ifTrue: [newSize := 0].
    result := Array new: newSize.
    1 to: newSize do: [:i |
        result at: i put: (self at: start + i - 1)].
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
    | count result j |
    count := 0.
    1 to: self size do: [:i |
        (self at: i) = anElement ifFalse: [count := count + 1]].
    result := Array new: count.
    j := 1.
    1 to: self size do: [:i |
        (self at: i) = anElement
            ifFalse: [result at: j put: (self at: i). j := j + 1]].
    ^ result! !
