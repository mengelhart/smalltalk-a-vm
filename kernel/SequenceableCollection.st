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
        i := i + 1]
!

collect: aBlock
    | result i sz |
    sz := self size.
    result := Array new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (aBlock value: (self at: i)).
        i := i + 1].
    ^ result
!

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
    ^ result
!

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
    ^ result
!

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
    ^ result
! !

!SequenceableCollection methodsFor: 'testing'!
includes: anObject
    | found i sz |
    found := false.
    i := 1.
    sz := self size.
    [i <= sz] whileTrue: [
        (self at: i) = anObject ifTrue: [found := true].
        i := i + 1].
    ^ found
! !

!SequenceableCollection methodsFor: 'accessing'!
first
    ^ self at: 1
!

last
    ^ self at: self size
! !
